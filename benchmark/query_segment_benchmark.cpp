/// Benchmark: Neo QuerySegment baseline re-query vs two DAG-reuse designs
///
/// Loads a Chinese text file, pre-decodes every line to Unicode, verifies that
/// the baseline implementation (`cut_with_requery`) and the two DAG-based
/// implementations (`cut_with_mix_dag`, `cut_with_inline_dag`) produce
/// identical output, then compares their throughput.

#include "neo/Utf.hpp"
#include "neo/algorithm/QuerySegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"

#include "BenchmarkUtils.hpp"
#include "GoogleBenchmarkUtils.hpp"
#include "QuerySegmentCompare.hpp"
#include "test_paths.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

namespace neo = neo_cppjieba;

// Throw unless the baseline and both DAG-reuse designs cut every line identically; the first differences are printed.
auto verify_designs(const neo::detail::DictTrie &dict, const neo::detail::HMModel &model,
                    const std::vector<neo::detail::Unicode> &all_unicodes) -> void {
    auto mismatch = size_t{0};
    for (auto i = size_t{0}; i < all_unicodes.size(); ++i) {
        auto baseline_result = neo::test::query_cut_requery(dict, model, all_unicodes[i]);
        auto buffered_result = neo::test::query_cut_buffered_dag(dict, model, all_unicodes[i]);
        auto inline_result = neo::detail::cut<neo::detail::QuerySegment<>>(dict, model, all_unicodes[i]);
        if (baseline_result != buffered_result || baseline_result != inline_result) {
            ++mismatch;
            if (mismatch <= 3) {
                auto input = neo::Utf::encode<char>(all_unicodes[i]);
                std::printf("  MISMATCH line %zu: \"%.*s\"\n", i, static_cast<int>(std::min(input.size(), size_t{80})),
                            input.data());
            }
        }
    }
    if (mismatch != 0) {
        throw std::runtime_error("Verification failed: " + std::to_string(mismatch) + " lines differ");
    }
}

// Decode the corpus, load the dictionary and model, verify the designs, then time each one.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 3, "Usage: query_segment_benchmark [--benchmark_<flag>...] [dict [model [text]]]");
    const auto dict_path = positional_argument(args, 1, std::string(DICT_DIR) + "/jieba.dict.utf8");
    const auto model_path = positional_argument(args, 2, std::string(DICT_DIR) + "/hmm_model.utf8");
    const auto text_path = positional_argument(args, 3, std::string(TEST_DATA_DIR) + "/weicheng.utf8");
    const auto lines = require_lines(text_path);

    auto all_unicodes = std::vector<neo::detail::Unicode>{};
    all_unicodes.reserve(lines.size());
    auto total_runes = size_t{0};
    for (const auto &line : lines) {
        all_unicodes.push_back(neo::Utf::decode(line));
        total_runes += all_unicodes.back().size();
    }

    const auto t0 = Clock::now();
    const auto dict = neo::detail::DictTrie{dict_path, "", neo::detail::UserWordWeightOption::WordWeightMedian};
    const auto model = neo::detail::HMModel{model_path};
    const auto t1 = Clock::now();
    verify_designs(dict, model, all_unicodes);

    benchmark::AddCustomContext("dictionary", dict_path);
    benchmark::AddCustomContext("hmm_model", model_path);
    benchmark::AddCustomContext("text", text_path);
    benchmark::AddCustomContext("lines", std::to_string(lines.size()));
    benchmark::AddCustomContext("runes", std::to_string(total_runes));
    benchmark::AddCustomContext("neo_load_ms", std::to_string(Ms(t1 - t0).count()));

    // QuerySegment baseline (Mix + dict.find 2/3-gram)
    benchmark::RegisterBenchmark("QuerySegment/requery", [&](benchmark::State &state) {
        time_corpus_cut(
            state, [&](const neo::detail::Unicode &runes) { return neo::test::query_cut_requery(dict, model, runes); },
            all_unicodes, total_runes);
    })->Unit(benchmark::kMillisecond);
    // QuerySegment DAG reuse (buffered Mix DAG + edge lookup)
    benchmark::RegisterBenchmark("QuerySegment/buffered_dag", [&](benchmark::State &state) {
        time_corpus_cut(
            state,
            [&](const neo::detail::Unicode &runes) { return neo::test::query_cut_buffered_dag(dict, model, runes); },
            all_unicodes, total_runes);
    })->Unit(benchmark::kMillisecond);
    // QuerySegment DAG reuse (inline segment consume)
    benchmark::RegisterBenchmark("QuerySegment/inline_dag", [&](benchmark::State &state) {
        time_corpus_cut(
            state,
            [&](const neo::detail::Unicode &runes) {
                return neo::detail::cut<neo::detail::QuerySegment<>>(dict, model, runes);
            },
            all_unicodes, total_runes);
    })->Unit(benchmark::kMillisecond);

    // COMPARISON (speedup = baseline time / DAG reuse): QuerySegment/requery over each DAG-reuse row.
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
