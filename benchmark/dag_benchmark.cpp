/// Benchmark: Old DAG construction vs Neo DAG construction
///
/// Loads a Chinese text file, decodes each line to runes, then compares
/// the DAG-building throughput of the old cppjieba::Trie::Find(begin, end, dags)
/// versus the new neo_cppjieba::detail::Trie::find_dag(runes).
///
/// Both use the same underlying dictionary (jieba.dict.utf8).

// ── Old headers ──
#include "cppjieba/DictTrie.hpp"
#include "cppjieba/Unicode.hpp"

// ── Neo headers ──
#include "neo/Utf.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"

#include "GoogleBenchmarkUtils.hpp"
#include "test_paths.h"

#include <cstddef>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

// ─────────────────────────────────────────────────────────────────────────────
// Anti-optimization barrier
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Old DAG construction (cppjieba::Trie::Find → vector<Dag>)
// ─────────────────────────────────────────────────────────────────────────────
// Holds the old trie and every line decoded to its rune array before timing.
class OldDag {
public:
    OldDag(const std::string &dict_path, const std::vector<std::string> &lines)
        : dict_trie_{dict_path}, rune_arrays_(lines.size()) {
        // Pre-decode all lines to RuneStrArray (excluded from timing)
        for (auto i = std::size_t{0}; i < lines.size(); ++i) {
            cppjieba::DecodeUTF8RunesInString(lines[i], rune_arrays_[i]);
            runes_ += rune_arrays_[i].size();
        }
    }
    OldDag(const OldDag &) = delete;
    auto operator=(const OldDag &) -> OldDag & = delete;

    auto run(benchmark::State &state) const -> void {
        // Timed run
        for ([[maybe_unused]]
             auto _ : state) {
            auto dags = std::vector<cppjieba::Dag>{};
            for (const auto &rune_array : rune_arrays_) {
                dict_trie_.Find(rune_array.begin(), rune_array.end(), dags);
                benchmark::DoNotOptimize(dags);
            }
        }
        set_corpus_rates(state, rune_arrays_.size(), runes_);
    }

private:
    cppjieba::DictTrie dict_trie_;
    std::vector<cppjieba::RuneStrArray> rune_arrays_;
    std::size_t runes_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Neo DAG construction (neo_cppjieba::detail::Trie::find_dag → Dag)
// ─────────────────────────────────────────────────────────────────────────────
// Holds the Neo trie and every line decoded to Unicode before timing.
class NeoDag {
public:
    NeoDag(const std::string &dict_path, const std::vector<std::string> &lines)
        : dict_trie_{dict_path, "", neo_cppjieba::detail::UserWordWeightOption::WordWeightMedian} {
        // Pre-decode all lines to Unicode (excluded from timing)
        rune_vecs_.reserve(lines.size());
        for (const auto &line : lines) {
            rune_vecs_.push_back(neo_cppjieba::Utf::decode(line));
            runes_ += rune_vecs_.back().size();
        }
    }
    NeoDag(const NeoDag &) = delete;
    auto operator=(const NeoDag &) -> NeoDag & = delete;

    auto run(benchmark::State &state) const -> void {
        const auto &trie = dict_trie_.trie();
        // Timed run
        for ([[maybe_unused]]
             auto _ : state) {
            for (const auto &runes : rune_vecs_) {
                auto dag = trie.find_dag(runes);
                benchmark::DoNotOptimize(dag);
            }
        }
        set_corpus_rates(state, rune_vecs_.size(), runes_);
    }

private:
    neo_cppjieba::detail::DictTrie dict_trie_;
    std::vector<neo_cppjieba::detail::Unicode> rune_vecs_;
    std::size_t runes_ = 0;
};

// Build both tries and decode the corpus, then let Google Benchmark time each engine.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 2, "Usage: dag_benchmark [--benchmark_<flag>...] [dict [text]]");
    const auto dict_path = positional_argument(args, 1, std::string{DICT_DIR} + "/jieba.dict.utf8");
    const auto text_path = positional_argument(args, 2, std::string{TEST_DATA_DIR} + "/weicheng.utf8");
    const auto lines = require_lines(text_path);
    benchmark::AddCustomContext("dictionary", dict_path);
    benchmark::AddCustomContext("text", text_path);
    benchmark::AddCustomContext("lines", std::to_string(lines.size()));

    // ── Old DAG ──────────────────────────────────────────────────────────
    // Old DAG (vector<Dag> + LocalVector<pair>)
    const auto old_dag = OldDag{dict_path, lines};
    // Warm up: Google Benchmark's unreported iteration-count estimation runs precede every timed run.
    benchmark::RegisterBenchmark("DAG/old", [&old_dag](benchmark::State &state) {
        old_dag.run(state);
    })->Unit(benchmark::kMillisecond);

    // ── Neo DAG ──────────────────────────────────────────────────────────
    // Neo DAG (flat CSR: offsets + edges)
    const auto neo_dag = NeoDag{dict_path, lines};
    // Warm up: Google Benchmark's unreported iteration-count estimation runs precede every timed run.
    benchmark::RegisterBenchmark("DAG/neo", [&neo_dag](benchmark::State &state) {
        neo_dag.run(state);
    })->Unit(benchmark::kMillisecond);

    // ── Comparison ───────────────────────────────────────────────────────
    // Speedup = DAG/old time / DAG/neo time, read from the two rows Google Benchmark reports.
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
