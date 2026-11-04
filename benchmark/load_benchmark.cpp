/// Benchmark: dictionary loading from text versus snapshots
///
/// Times each stage a service pays at startup, over several rounds:
///
///   1. Text DictTrie        — parse the main and user dictionaries and build the trie word by word
///   2. Snapshot decode      — check and decode snapshot bytes into the level-order dictionary
///   3. Snapshot DictTrie    — decode and build from memory, then from a file read into owned memory
///   4. HMModel              — the model every Jieba also loads
///   5. Jieba                — complete construction from text and from a snapshot file
///
/// With --cold every input file is evicted from the page cache before each round (Linux only), approximating a
/// service starting after its files left the cache. Timings are reported only after snapshot and text loading
/// cut the corpus identically.

#include "neo/Jieba.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictSnapshot.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"

#include "BenchmarkUtils.hpp"
#include "GoogleBenchmarkUtils.hpp"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace neo_cppjieba;

namespace {

// Whether each round first evicts its input files from the page cache.
enum class PageCache { Warm, Cold };

/// Command-line settings.
struct Settings {
    PageCache cache;
    int rounds;
};

// Removes the snapshot directory on every exit path, including a failed comparison.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::filesystem::path path) : path_{std::move(path)} {
        std::filesystem::create_directories(path_);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    auto operator=(const TemporaryDirectory &) -> TemporaryDirectory & = delete;
    ~TemporaryDirectory() {
        auto error = std::error_code{};
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]]
    auto path() const -> const std::filesystem::path & {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Drop the clean cached pages of a file so the next read comes from storage; only Linux allows this without root.
auto evict_from_page_cache(const std::filesystem::path &path) -> bool {
#ifdef __linux__
    const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return false;
    }
    const auto advised = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
    ::close(fd);
    return advised;
#else
    static_cast<void>(path);
    return false;
#endif
}

// Smallest and largest round, reported beside Google Benchmark's mean, median and spread.
auto minimum(const std::vector<double> &rounds) -> double {
    return *std::ranges::min_element(rounds);
}

auto maximum(const std::vector<double> &rounds) -> double {
    return *std::ranges::max_element(rounds);
}

// Time one case; its result is destroyed after the clock stops, so teardown stays out of the timing.
// Each round is one repetition of a single load, so every round evicts its inputs first in cold mode.
template <typename Load>
auto register_case(const char *name, const Settings &settings, std::vector<std::filesystem::path> inputs, Load load)
    -> void {
    benchmark::RegisterBenchmark(name,
                                 [cache = settings.cache, inputs = std::move(inputs), load](benchmark::State &state) {
                                     for ([[maybe_unused]]
                                          auto _ : state) {
                                         if (cache == PageCache::Cold) {
                                             std::ranges::for_each(inputs, evict_from_page_cache);
                                         }
                                         const auto start = Clock::now();
                                         auto result = load();
                                         state.SetIterationTime(elapsed_seconds(start));
                                         benchmark::DoNotOptimize(result);
                                     }
                                 })
        ->UseManualTime()
        ->Iterations(1)
        ->Repetitions(settings.rounds)
        ->ReportAggregatesOnly(true)
        ->ComputeStatistics("min", minimum)
        ->ComputeStatistics("max", maximum)
        ->Unit(benchmark::kMillisecond);
}

// Parse [--cold] [--rounds N]; throws usage for anything else.
auto parse_settings(BenchmarkArgs args) -> Settings {
    auto settings = Settings{PageCache::Warm, 9};
    for (auto i = std::size_t{1}; i < args.size(); ++i) {
        const auto arg = std::string_view{args[i]};
        if (arg == "--cold") {
            settings.cache = PageCache::Cold;
            continue;
        }
        if (arg == "--rounds" && i + 1 < args.size()) {
            const auto value = std::string_view{args[++i]};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), settings.rounds);
            if (error == std::errc{} && end == value.data() + value.size() && settings.rounds > 0) {
                continue;
            }
        }
        throw std::invalid_argument("usage: load_benchmark [--benchmark_<flag>...] [--cold] [--rounds N]");
    }
    return settings;
}

auto write_file(const std::filesystem::path &path, std::span<const std::byte> bytes) -> void {
    auto output = std::ofstream{path, std::ios::binary};
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Snapshot and text loading must cut and tag the corpus identically before their timings mean anything.
auto same_cuts(const Jieba &snapshot, const Jieba &text, std::span<const std::string> lines) -> bool {
    return std::ranges::all_of(lines, [&](const std::string &line) {
        return snapshot.cut_strings<Tagging::Pos>(line, CutMode::MIX)
               == text.cut_strings<Tagging::Pos>(line, CutMode::MIX);
    });
}

auto megabytes(std::uintmax_t bytes) -> double {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// Encode a snapshot, check it against text loading, then time every startup stage.
auto run(BenchmarkArgs args) -> void {
    const auto settings = parse_settings(args);
    const auto dict_path = std::filesystem::path{DICT_DIR "/jieba.dict.utf8"};
    const auto user_path = std::filesystem::path{DICT_DIR "/user.dict.utf8"};
    const auto model_path = std::filesystem::path{DICT_DIR "/hmm_model.utf8"};
    const auto dict = DictSource::file(dict_path.string());
    const auto user_dicts = std::array{DictSource::file(user_path.string())};
    const auto model = DictSource::file(model_path.string());
    const auto opt = detail::UserWordWeightOption::WordWeightMedian;

    const auto encode_start = Clock::now();
    const auto bytes = Jieba::encode_dict_snapshot(dict, user_dicts);
    const auto encode_ms = Ms{Clock::now() - encode_start}.count();
    const auto directory = TemporaryDirectory{std::filesystem::temp_directory_path() / "neo-load-benchmark"};
    const auto snapshot_path = directory.path() / "jieba.snapshot";
    write_file(snapshot_path, bytes);
    const auto snapshot = SnapshotSource::file(snapshot_path.string());

    const auto snapshot_jieba = Jieba::from_snapshot(snapshot, model);
    const auto text_jieba = Jieba{dict, model, user_dicts};
    const auto lines = require_lines(TEST_DATA_DIR "/weicheng.utf8");
    if (!same_cuts(snapshot_jieba, text_jieba, lines)) {
        throw std::runtime_error("snapshot and text loading cut the corpus differently");
    }

    const auto cold = settings.cache == PageCache::Cold;
    if (cold && !evict_from_page_cache(dict_path)) {
        std::printf("Page cache eviction is unavailable here; the cold run measures a warm cache.\n");
    }
    benchmark::AddCustomContext("main_dictionary_mib",
                                std::to_string(megabytes(std::filesystem::file_size(dict_path))));
    benchmark::AddCustomContext("user_dictionary_bytes", std::to_string(std::filesystem::file_size(user_path)));
    benchmark::AddCustomContext("snapshot_mib", std::to_string(megabytes(bytes.size())));
    benchmark::AddCustomContext("snapshot_encode_ms", std::to_string(encode_ms));
    benchmark::AddCustomContext("page_cache", cold ? "cold" : "warm");
    benchmark::AddCustomContext("corpus_lines_matched", std::to_string(lines.size()));

    const auto memory = SnapshotSource::memory(bytes, "jieba.snapshot");
    register_case("Load/text_dict_trie", settings, {dict_path, user_path},
                  [&] { return detail::DictTrie{dict, user_dicts, opt}; });
    register_case("Load/snapshot_decode_memory", settings, {},
                  [&] { return detail::decode_dict_snapshot(bytes, "jieba.snapshot"); });
    register_case("Load/snapshot_dict_trie_memory", settings, {}, [&] { return detail::load_dict_snapshot(memory); });
    register_case("Load/snapshot_dict_trie_file", settings, {snapshot_path},
                  [&] { return detail::load_dict_snapshot(snapshot); });
    register_case("Load/hmm_model", settings, {model_path}, [&] { return detail::HMModel{model}; });
    register_case("Load/jieba_text", settings, {dict_path, user_path, model_path},
                  [&] { return Jieba{dict, model, user_dicts}; });
    register_case("Load/jieba_snapshot_file", settings, {snapshot_path, model_path},
                  [&] { return Jieba::from_snapshot(snapshot, model); });
    // Jieba construction speedup (median): Load/jieba_text_median over Load/jieba_snapshot_file_median.
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
