/// Benchmark: Old DictTrie vs Neo DictTrie — query DictUnit performance
///
/// Builds both DictTrie objects from jieba.dict.utf8, pre-decodes all query
/// keys to Unicode *before* timing, then compares pure lookup throughput:
///
///   1. Sequential all-hit   — query every dict word in order
///   2. Random all-hit       — query every dict word in shuffled order
///   3. All-miss             — query words guaranteed not in the dict
///   4. Short miss           — single-rune queries that don't exist
///
/// The Unicode transcoding cost is explicitly excluded from all measurements.

// ── Old DictTrie (cppjieba namespace) ──
#include "cppjieba/DictTrie.hpp"

// ── Neo DictTrie (neo_cppjieba namespace) ──
#include "neo/Utf.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"

#include "BenchmarkUtils.hpp"
#include "GoogleBenchmarkUtils.hpp"
#include "test_paths.h"

#include <array>
#include <cstddef>
#include <cstdint>
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
// Load raw UTF-8 words from dict file (word only, for query key generation)
// ─────────────────────────────────────────────────────────────────────────────
static auto load_words(const std::string &dict_path) -> std::vector<std::string> {
    auto words = std::vector<std::string>{};
    words.reserve(350000);

    for (const std::string_view line : load_lines(dict_path)) {
        auto sp = line.find(' ');
        if (sp == std::string_view::npos) {
            continue;
        }
        words.emplace_back(line.substr(0, sp));
    }

    return words;
}

// Register the four lookup kinds of one DictTrie under "<kind>/<name>"; find, keys and order outlive the run.
template <typename FindFn, typename Key>
auto register_lookups(const char *name, const FindFn &find, const std::vector<Key> &keys,
                      const std::vector<size_t> &shuffled, const std::vector<Key> &miss_keys,
                      const std::vector<Key> &short_miss_keys) -> void {
    const auto label = std::string{name};
    // ── Sequential query (all-hit) ───────────────────────────────────────
    benchmark::RegisterBenchmark("SeqHit/" + label, [find, &keys](benchmark::State &state) {
        time_lookups(state, find, keys);
    })->Unit(benchmark::kMillisecond);
    // ── Random query (all-hit) ───────────────────────────────────────────
    benchmark::RegisterBenchmark("RandHit/" + label, [find, &keys, &shuffled](benchmark::State &state) {
        time_lookups_in_order(state, find, keys, shuffled);
    })->Unit(benchmark::kMillisecond);
    // ── Miss query (mutated first rune) ──────────────────────────────────
    benchmark::RegisterBenchmark("Miss/" + label, [find, &miss_keys](benchmark::State &state) {
        time_lookups(state, find, miss_keys);
    })->Unit(benchmark::kMillisecond);
    // ── Short miss query (single rune) ───────────────────────────────────
    benchmark::RegisterBenchmark("ShortMiss/" + label, [find, &short_miss_keys](benchmark::State &state) {
        time_lookups(state, find, short_miss_keys);
    })->Unit(benchmark::kMillisecond);
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Old DictTrie (cppjieba::DictTrie)
// ─────────────────────────────────────────────────────────────────────────────
// Holds the old DictTrie and its query keys, all decoded before timing.
class OldDictTrieBench {
public:
    OldDictTrieBench(const std::string &dict_path, const std::vector<std::string> &words)
        // Build DictTrie (not timed — we only care about query performance)
        : dict_trie_(dict_path), rune_arrays_(words.size()), miss_arrays_(words.size()),
          short_miss_arrays_(words.size()) {
        // ── Pre-decode all query keys (excluded from timing) ─────────────────
        for (auto i = size_t{0}; i < words.size(); ++i) {
            old::DecodeUTF8RunesInString(words[i], rune_arrays_[i]);
        }

        // Shuffled indices
        shuffled_ = shuffled_indices(words.size(), kShuffleSeed);

        // Miss keys: replace the first rune with a code point that won't appear in any dict word
        for (auto i = size_t{0}; i < words.size(); ++i) {
            miss_arrays_[i] = rune_arrays_[i];
            if (!miss_arrays_[i].empty()) {
                miss_arrays_[i][0].rune = 0x10FFFF;
            }
        }

        // Short miss keys: single rune that is unlikely to be in the dict
        constexpr auto kMissRunes = std::array<old::Rune, 4>{0x10FFFE, 0x10FFFD, 0x10FFFC, 0x10FFFB};
        for (auto i = size_t{0}; i < words.size(); ++i) {
            old::RuneStr rs;
            rs.rune = kMissRunes[i % 4];
            rs.offset = 0;
            rs.len = 1;
            rs.unicode_offset = 0;
            rs.unicode_length = 1;
            short_miss_arrays_[i].push_back(rs);
        }
    }
    OldDictTrieBench(const OldDictTrieBench &) = delete;
    auto operator=(const OldDictTrieBench &) -> OldDictTrieBench & = delete;

    auto register_benchmarks() const -> void {
        const auto find = [this](const old::RuneStrArray &ra) {
            return dict_trie_.Find(ra.begin(), ra.end());
        };
        register_lookups("old", find, rune_arrays_, shuffled_, miss_arrays_, short_miss_arrays_);
    }

private:
    old::DictTrie dict_trie_;
    std::vector<old::RuneStrArray> rune_arrays_;
    std::vector<size_t> shuffled_;
    std::vector<old::RuneStrArray> miss_arrays_;
    std::vector<old::RuneStrArray> short_miss_arrays_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Neo DictTrie (neo_cppjieba::detail::DictTrie)
// ─────────────────────────────────────────────────────────────────────────────
// Holds the Neo DictTrie and its query keys, all decoded before timing.
class NeoDictTrieBench {
public:
    NeoDictTrieBench(const std::string &dict_path, const std::vector<std::string> &words)
        // Build DictTrie (not timed)
        : dict_trie_{dict_path, "", neo::detail::UserWordWeightOption::WordWeightMedian}, keys_(words.size()),
          miss_keys_(words.size()), short_miss_keys_(words.size()) {
        // ── Pre-decode all query keys (excluded from timing) ─────────────────
        for (auto i = size_t{0}; i < words.size(); ++i) {
            keys_[i] = neo::Utf::decode(words[i]);
        }

        // Shuffled indices
        shuffled_ = shuffled_indices(words.size(), kShuffleSeed);

        // Miss keys: replace the first rune
        for (auto i = size_t{0}; i < words.size(); ++i) {
            miss_keys_[i] = keys_[i];
            if (!miss_keys_[i].empty()) {
                miss_keys_[i][0] = 0x10FFFF;
            }
        }

        // Short miss keys: single rune
        constexpr auto kMissRunes = std::array<neo::Rune, 4>{0x10FFFE, 0x10FFFD, 0x10FFFC, 0x10FFFB};
        for (auto i = size_t{0}; i < words.size(); ++i) {
            short_miss_keys_[i] = {kMissRunes[i % 4]};
        }
    }
    NeoDictTrieBench(const NeoDictTrieBench &) = delete;
    auto operator=(const NeoDictTrieBench &) -> NeoDictTrieBench & = delete;

    auto register_benchmarks() const -> void {
        const auto find = [this](const neo::detail::Unicode &key) {
            return dict_trie_.find(std::span<const neo::Rune>{key});
        };
        register_lookups("neo", find, keys_, shuffled_, miss_keys_, short_miss_keys_);
    }

private:
    neo::detail::DictTrie dict_trie_;
    std::vector<neo::detail::Unicode> keys_;
    std::vector<size_t> shuffled_;
    std::vector<neo::detail::Unicode> miss_keys_;
    std::vector<neo::detail::Unicode> short_miss_keys_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

// Load the query words, build both DictTries, then time each lookup kind.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 1, "Usage: dict_trie_benchmark [--benchmark_<flag>...] [dict]");
    const auto dict_path = positional_argument(args, 1, std::string(DICT_DIR) + "/jieba.dict.utf8");
    const auto words = load_words(dict_path);
    if (words.empty()) {
        throw std::runtime_error("no dictionary words loaded from " + dict_path);
    }
    benchmark::AddCustomContext("dictionary", dict_path);
    benchmark::AddCustomContext("query_words", std::to_string(words.size()));

    // ── Old DictTrie ─────────────────────────────────────────────────────
    // Old DictTrie (unordered_map per TrieNode, pointer)
    const auto old_bench = OldDictTrieBench{dict_path, words};
    old_bench.register_benchmarks();

    // ── Neo DictTrie ─────────────────────────────────────────────────────
    // Neo DictTrie (flat vector, BFS layout, inline value)
    const auto neo_bench = NeoDictTrieBench{dict_path, words};
    neo_bench.register_benchmarks();

    // ── Comparison ───────────────────────────────────────────────────────
    // speedup = Old time / Neo time: compare each <kind>/old row with <kind>/neo.
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
