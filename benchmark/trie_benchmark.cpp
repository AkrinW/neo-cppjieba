/// Benchmark: Old Trie vs Neo Trie
///
/// Reads jieba.dict.utf8, builds both tries, then compares:
///   1. Build time
///   2. Query throughput (all-hit sequential scan + random-order scan + miss queries)
///
/// Compile:
///   cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCPPJIEBA_BUILD_BENCHMARKS=ON
///   cmake --build build-bench --target trie_benchmark
///
/// Run:
///   ./build-bench/benchmark/trie_benchmark [--benchmark_<flag>...] [path/to/jieba.dict.utf8]

// ── Old Trie headers (defines cppjieba::Rune as uint32_t, cppjieba::Unicode as LocalVector<Rune>) ──
#include "cppjieba/Trie.hpp"
#include "cppjieba/Unicode.hpp"

// ── Neo Trie header (neo_cppjieba namespace — no conflict with cppjieba) ──
#include "neo/Utf.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/Trie.hpp"

#include "BenchmarkUtils.hpp"
#include "GoogleBenchmarkUtils.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

namespace old = cppjieba;
namespace neo = neo_cppjieba;

// Seed of the shuffled all-hit query order, shared by both tries.
constexpr auto kShuffleSeed = std::uint32_t{42};

// ─────────────────────────────────────────────────────────────────────────────
// Dictionary loading — shared raw data
// ─────────────────────────────────────────────────────────────────────────────

struct RawEntry {
    std::string word;
    double freq;
    std::string tag;
};

static auto load_raw_entries(const std::string &dict_path) -> std::vector<RawEntry> {
    auto entries = std::vector<RawEntry>{};
    entries.reserve(350000);

    for (const std::string_view line : load_lines(dict_path)) {
        auto sp1 = line.find(' ');
        if (sp1 == std::string_view::npos) {
            continue;
        }
        auto sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos) {
            continue;
        }

        auto freq = 0.0;
        for (auto c : line.substr(sp1 + 1, sp2 - sp1 - 1)) {
            if (c >= '0' && c <= '9') {
                freq = freq * 10 + (c - '0');
            }
        }
        entries.push_back({
            std::string(line.substr(0, sp1)),
            freq,
            std::string(line.substr(sp2 + 1)),
        });
    }

    return entries;
}

// Sum of all entry frequencies, the denominator of each entry's log weight.
auto frequency_sum(const std::vector<RawEntry> &entries) -> double {
    return std::accumulate(entries.begin(), entries.end(), 0.0,
                           [](double sum, const RawEntry &entry) { return sum + entry.freq; });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Old Trie (cppjieba::Trie)
// ─────────────────────────────────────────────────────────────────────────────
// Holds the build inputs and decoded query keys; the query benchmarks share one trie built at construction.
class OldTrieBench {
public:
    explicit OldTrieBench(const std::vector<RawEntry> &entries)
        : dict_units_(entries.size()), rune_arrays_(entries.size()), miss_rune_arrays_(entries.size()) {
        const auto freq_sum = frequency_sum(entries);
        for (auto i = size_t{0}; i < entries.size(); ++i) {
            auto runes = old::RuneStrArray{};
            old::DecodeUTF8RunesInString(entries[i].word, runes);
            dict_units_[i].word.clear();
            for (const auto &runeStr : runes) {
                dict_units_[i].word.push_back(runeStr.rune);
            }
            dict_units_[i].weight = entries[i].freq > 0 ? std::log(entries[i].freq / freq_sum) : -20.0;
            dict_units_[i].tag = entries[i].tag;
        }

        keys_.reserve(dict_units_.size());
        ptrs_.reserve(dict_units_.size());
        for (const auto &unit : dict_units_) {
            keys_.push_back(unit.word);
            ptrs_.push_back(&unit);
        }

        for (auto i = size_t{0}; i < entries.size(); ++i) {
            old::DecodeUTF8RunesInString(entries[i].word, rune_arrays_[i]);
        }
        shuffled_ = shuffled_indices(entries.size(), kShuffleSeed);
        // Copy miss keys in their own pass so their heap blocks stay contiguous; interleaving them with the hit keys
        // spread each miss scan over more pages and doubled its time.
        for (auto i = size_t{0}; i < entries.size(); ++i) {
            miss_rune_arrays_[i] = rune_arrays_[i];
            if (!miss_rune_arrays_[i].empty()) {
                miss_rune_arrays_[i][0].rune = 0x10FFFF;
            }
        }
        trie_ = std::make_unique<old::Trie>(keys_, ptrs_);
    }
    OldTrieBench(const OldTrieBench &) = delete;
    auto operator=(const OldTrieBench &) -> OldTrieBench & = delete;

    auto register_benchmarks() const -> void {
        // ── Build ────────────────────────────────────────────────────────────
        benchmark::RegisterBenchmark("Build/old",
                                     [this](benchmark::State &state) {
                                         for ([[maybe_unused]]
                                              auto _ : state) {
                                             const auto start = Clock::now();
                                             auto trie = std::make_unique<old::Trie>(keys_, ptrs_);
                                             state.SetIterationTime(elapsed_seconds(start));
                                             benchmark::DoNotOptimize(trie);
                                         }
                                     })
            ->UseManualTime()
            ->Unit(benchmark::kMillisecond);
        const auto find = [this](const old::RuneStrArray &ra) {
            return trie_->Find(ra.begin(), ra.end());
        };
        // ── Sequential query (all-hit) ───────────────────────────────────────
        benchmark::RegisterBenchmark("SeqHit/old", [this, find](benchmark::State &state) {
            time_lookups(state, find, rune_arrays_);
        })->Unit(benchmark::kMillisecond);
        // ── Random query (all-hit) ───────────────────────────────────────────
        benchmark::RegisterBenchmark("RandHit/old", [this, find](benchmark::State &state) {
            time_lookups_in_order(state, find, rune_arrays_, shuffled_);
        })->Unit(benchmark::kMillisecond);
        // ── Miss query ───────────────────────────────────────────────────────
        benchmark::RegisterBenchmark("Miss/old", [this, find](benchmark::State &state) {
            time_lookups(state, find, miss_rune_arrays_);
        })->Unit(benchmark::kMillisecond);
    }

private:
    std::vector<old::DictUnit> dict_units_;
    std::vector<old::Unicode> keys_;
    std::vector<const old::DictUnit *> ptrs_;
    std::vector<old::RuneStrArray> rune_arrays_;
    std::vector<old::RuneStrArray> miss_rune_arrays_;
    std::vector<size_t> shuffled_;
    std::unique_ptr<old::Trie> trie_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Neo Trie (using neo_cppjieba::detail::Trie from neo/resource/Trie.hpp)
// ─────────────────────────────────────────────────────────────────────────────
// Holds the build inputs and miss keys; the query benchmarks share one trie built at construction.
class NeoTrieBench {
public:
    explicit NeoTrieBench(const std::vector<RawEntry> &entries)
        : keys_(entries.size()), values_(entries.size()), miss_keys_(entries.size()) {
        const auto freq_sum = frequency_sum(entries);
        for (auto i = size_t{0}; i < entries.size(); ++i) {
            keys_[i] = neo::Utf::decode(entries[i].word);
            auto w = entries[i].freq > 0 ? static_cast<float>(std::log(entries[i].freq / freq_sum)) : -20.0f;
            values_[i] = {w, neo::PosTag(entries[i].tag)};
        }
        shuffled_ = shuffled_indices(entries.size(), kShuffleSeed);
        // Copy miss keys in their own pass so their heap blocks stay contiguous, as in OldTrieBench.
        for (auto i = size_t{0}; i < entries.size(); ++i) {
            miss_keys_[i] = keys_[i];
            if (!miss_keys_[i].empty()) {
                miss_keys_[i][0] = 0x10FFFF;
            }
        }
        trie_.build(keys_, values_);
    }
    NeoTrieBench(const NeoTrieBench &) = delete;
    auto operator=(const NeoTrieBench &) -> NeoTrieBench & = delete;

    auto register_benchmarks() const -> void {
        // ── Build ────────────────────────────────────────────────────────────
        benchmark::RegisterBenchmark("Build/neo",
                                     [this](benchmark::State &state) {
                                         for ([[maybe_unused]]
                                              auto _ : state) {
                                             auto trie = neo::detail::Trie{};
                                             const auto start = Clock::now();
                                             trie.build(keys_, values_);
                                             state.SetIterationTime(elapsed_seconds(start));
                                             benchmark::DoNotOptimize(trie);
                                         }
                                     })
            ->UseManualTime()
            ->Unit(benchmark::kMillisecond);
        const auto find = [this](const neo::detail::Unicode &key) {
            return trie_.find(std::span<const neo::Rune>{key});
        };
        // ── Sequential query (all-hit) ───────────────────────────────────────
        benchmark::RegisterBenchmark("SeqHit/neo", [this, find](benchmark::State &state) {
            time_lookups(state, find, keys_);
        })->Unit(benchmark::kMillisecond);
        // ── Random query (all-hit) ───────────────────────────────────────────
        benchmark::RegisterBenchmark("RandHit/neo", [this, find](benchmark::State &state) {
            time_lookups_in_order(state, find, keys_, shuffled_);
        })->Unit(benchmark::kMillisecond);
        // ── Miss query ───────────────────────────────────────────────────────
        benchmark::RegisterBenchmark("Miss/neo", [this, find](benchmark::State &state) {
            time_lookups(state, find, miss_keys_);
        })->Unit(benchmark::kMillisecond);
    }

private:
    std::vector<neo::detail::Unicode> keys_;
    std::vector<neo::detail::DictUnit> values_;
    std::vector<neo::detail::Unicode> miss_keys_;
    std::vector<size_t> shuffled_;
    neo::detail::Trie trie_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

// Load the dictionary once, prepare both tries, then time their builds and queries.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 1, "Usage: trie_benchmark [--benchmark_<flag>...] [path/to/jieba.dict.utf8]");
    const auto dict_path = positional_argument(args, 1, "dict/jieba.dict.utf8");
    const auto entries = load_raw_entries(dict_path);
    if (entries.empty()) {
        throw std::runtime_error("no dictionary entries loaded from " + dict_path);
    }
    benchmark::AddCustomContext("dictionary", dict_path);
    benchmark::AddCustomContext("entries", std::to_string(entries.size()));

    // Old Trie (unordered_map, pointer-based)
    const auto old_bench = OldTrieBench{entries};
    old_bench.register_benchmarks();
    // Neo Trie (flat vector, BFS layout, binary search)
    const auto neo_bench = NeoTrieBench{entries};
    neo_bench.register_benchmarks();

    // ── Comparison ───────────────────────────────────────────────────────
    // speedup = Old time / Neo time: compare each <operation>/old row with <operation>/neo.
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
