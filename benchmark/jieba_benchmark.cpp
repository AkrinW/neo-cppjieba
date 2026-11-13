/// Benchmark: Compare five segmentation methods (Old vs Neo) via Jieba
///
/// Loads a Chinese text file, then:
///   1. Verifies that old (cppjieba) and neo (neo_cppjieba) produce identical
///      segmentation results for each of the five methods.
///   2. Benchmarks the Neo core segmentation throughput (Unicode-level,
///      pre-decoded, excludes decode/encode) through a single Jieba instance.
///   3. Benchmarks Old vs Neo at the string-level (full pipeline: decode →
///      pre-filter → cut → encode) for a fair comparison.
///
/// Five segmentation methods, plus MIX with part-of-speech tags (TAG):
///   MIX    — Dictionary (MP) + HMM  (recommended default)
///   MP     — Pure dictionary (maximum probability, no HMM)
///   HMM    — Pure HMM (Viterbi only, no dictionary)
///   FULL   — All dictionary-matching words (overlapping)
///   SEARCH — MIX + sub-word extraction (for search engines)
///   TAG    — MIX words with POS tags (old Jieba::Tag vs neo cut_strings<Tagging::Pos>)

// ── Old headers ──
#include "cppjieba/Jieba.hpp"

// ── Neo headers ──
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"

#include "BenchmarkUtils.hpp"
#include "GoogleBenchmarkUtils.hpp"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

using CM = neo_cppjieba::CutMode;

// One of the five segmentation methods, with the name both the Old and Neo benchmarks use for it.
struct Method {
    const char *name;
    CM mode;
};

constexpr auto kMethods = std::array{
    Method{"MIX", CM::MIX},   Method{"MP", CM::MP},         Method{"HMM", CM::HMM},
    Method{"FULL", CM::FULL}, Method{"SEARCH", CM::SEARCH},
};

// ─────────────────────────────────────────────────────────────────────────────
// Correctness verification — compare old vs neo for one method
//
// OldCutFn: (const std::string &) -> std::vector<std::string>
// NeoCutFn: (const std::string &) -> std::vector<std::string>
// ─────────────────────────────────────────────────────────────────────────────
template <typename OldCutFn, typename NeoCutFn>
static
auto verify_method(const char *method_name, const OldCutFn &old_fn, const NeoCutFn &neo_fn,
                   const std::vector<std::string> &lines) -> bool {
    std::printf("  %-8s: ", method_name);
    auto mismatches = size_t{0};
    for (auto i = size_t{0}; i < lines.size(); ++i) {
        auto old_words = old_fn(lines[i]);
        auto neo_words = neo_fn(lines[i]);
        if (old_words != neo_words) {
            ++mismatches;
            if (mismatches <= 3) {
                std::printf("\n    MISMATCH line %zu: \"%.*s\"\n", i,
                            static_cast<int>(std::min(lines[i].size(), size_t{60})), lines[i].data());
                std::printf("      Old (%zu):", old_words.size());
                for (auto j = size_t{0}; j < std::min(old_words.size(), size_t{15}); ++j) {
                    std::printf(" [%s]", old_words[j].c_str());
                }
                if (old_words.size() > 15) {
                    std::printf(" ...");
                }
                std::printf("\n");
                std::printf("      Neo (%zu):", neo_words.size());
                for (auto j = size_t{0}; j < std::min(neo_words.size(), size_t{15}); ++j) {
                    std::printf(" [%s]", neo_words[j].c_str());
                }
                if (neo_words.size() > 15) {
                    std::printf(" ...");
                }
                std::printf("\n    ");
            }
        }
    }
    if (mismatches == 0) {
        std::printf("✓ %zu lines identical\n", lines.size());
        return true;
    }
    std::printf("✗ %zu / %zu lines differ\n", mismatches, lines.size());
    return false;
}

// Tagged results compare as "word/tag" strings so verify_method can report mismatches.
static
auto old_tagged_words(const cppjieba::Jieba &jieba, const std::string &s) -> std::vector<std::string> {
    auto pairs = std::vector<std::pair<std::string, std::string>>{};
    jieba.Tag(s, pairs);
    auto words = std::vector<std::string>{};
    words.reserve(pairs.size());
    for (auto &[word, tag] : pairs) {
        words.push_back(word + "/" + tag);
    }
    return words;
}

static
auto neo_tagged_words(const neo_cppjieba::Jieba &jieba, const std::string &s) -> std::vector<std::string> {
    auto words = std::vector<std::string>{};
    for (auto &[word, tag] : jieba.cut_strings<neo_cppjieba::Tagging::Pos>(s, neo_cppjieba::CutMode::MIX)) {
        words.push_back(word + "/" + tag.to_string());
    }
    return words;
}

// Old Jieba's call for one method, chosen at compile time so each timed loop contains only that call.
template <CM Mode>
auto old_cut(const cppjieba::Jieba &jieba, const std::string &s) -> std::vector<std::string> {
    auto w = std::vector<std::string>{};
    if constexpr (Mode == CM::MIX) {
        jieba.Cut(s, w);
    } else if constexpr (Mode == CM::MP) {
        jieba.CutSmall(s, w, 99);
    } else if constexpr (Mode == CM::HMM) {
        jieba.CutHMM(s, w);
    } else if constexpr (Mode == CM::FULL) {
        jieba.CutAll(s, w);
    } else {
        static_assert(Mode == CM::SEARCH);
        jieba.CutForSearch(s, w);
    }
    return w;
}

// Compare Old and Neo string output for one method; returns whether every line matches.
template <CM Mode>
auto verify_mode(const char *name, const cppjieba::Jieba &old_jieba, const neo_cppjieba::Jieba &jieba,
                 const std::vector<std::string> &lines) -> bool {
    return verify_method(
        name, [&](const std::string &s) { return old_cut<Mode>(old_jieba, s); },
        [&](const std::string &s) { return jieba.cut_strings(s, Mode); }, lines);
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic benchmark runner (Unicode-level, pre-decoded)
//
// CutFn: (const Unicode &) -> std::vector<WordRange>
// ─────────────────────────────────────────────────────────────────────────────
auto register_core(const neo_cppjieba::Jieba &jieba, const std::vector<neo_cppjieba::detail::Unicode> &all_unicodes,
                   size_t total_runes) -> void {
    for (const auto &method : kMethods) {
        benchmark::RegisterBenchmark(std::string{"Core/"} + method.name, [&jieba, &all_unicodes, total_runes,
                                                                          mode = method.mode](benchmark::State &state) {
            time_corpus_cut(
                state, [&jieba, mode](const neo_cppjieba::detail::Unicode &u) { return jieba.cut_runes(u, mode); },
                all_unicodes, total_runes);
        })->Unit(benchmark::kMillisecond);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic benchmark runner (string-level, full pipeline)
//
// CutFn: (const std::string &) -> std::vector<std::string>
// ─────────────────────────────────────────────────────────────────────────────
template <typename OldCutFn, typename NeoCutFn>
auto register_string_pair(const char *name, const OldCutFn &old_fn, const NeoCutFn &neo_fn,
                          const std::vector<std::string> &lines, size_t total_runes) -> void {
    benchmark::RegisterBenchmark(std::string{"String/"} + name + "/old", [old_fn, &lines,
                                                                          total_runes](benchmark::State &state) {
        time_corpus_cut(state, old_fn, lines, total_runes);
    })->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark(std::string{"String/"} + name + "/neo", [neo_fn, &lines,
                                                                          total_runes](benchmark::State &state) {
        time_corpus_cut(state, neo_fn, lines, total_runes);
    })->Unit(benchmark::kMillisecond);
}

// Register the string-level Old and Neo benchmarks of one of the five methods.
template <CM Mode>
auto register_string_method(const char *name, const cppjieba::Jieba &old_jieba, const neo_cppjieba::Jieba &jieba,
                            const std::vector<std::string> &lines, size_t total_runes) -> void {
    register_string_pair(
        name, [&old_jieba](const std::string &s) { return old_cut<Mode>(old_jieba, s); },
        [&jieba](const std::string &s) { return jieba.cut_strings(s, Mode); }, lines, total_runes);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

// Load both Jieba instances, verify them against each other, then time the Neo core and both string pipelines.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 3, "Usage: jieba_benchmark [--benchmark_<flag>...] [dict [model [text]]]");
    const auto dict_path = positional_argument(args, 1, std::string(DICT_DIR) + "/jieba.dict.utf8");
    const auto model_path = positional_argument(args, 2, std::string(DICT_DIR) + "/hmm_model.utf8");
    const auto user_dict_path = std::string(DICT_DIR) + "/user.dict.utf8";
    const auto text_path = positional_argument(args, 3, std::string(TEST_DATA_DIR) + "/weicheng.utf8");

    // ── Load text ────────────────────────────────────────────────────────
    const auto lines = require_lines(text_path);

    // ── Pre-decode all lines to Unicode (excluded from neo core timing) ──
    auto all_unicodes = std::vector<neo_cppjieba::detail::Unicode>{};
    all_unicodes.reserve(lines.size());
    auto total_runes = size_t{0};
    for (const auto &line : lines) {
        all_unicodes.push_back(neo_cppjieba::Utf::decode(line));
        total_runes += all_unicodes.back().size();
    }

    // ── Construct Neo Jieba ──────────────────────────────────────────────
    auto t0 = Clock::now();
    const auto jieba = neo_cppjieba::Jieba{dict_path, model_path, user_dict_path};
    auto t1 = Clock::now();
    const auto neo_load_ms = Ms(t1 - t0).count();

    // ── Construct Old Jieba ──────────────────────────────────────────────
    t0 = Clock::now();
    const auto old_jieba = cppjieba::Jieba{dict_path, model_path, user_dict_path};
    t1 = Clock::now();
    const auto old_load_ms = Ms(t1 - t0).count();

    benchmark::AddCustomContext("dictionary", dict_path);
    benchmark::AddCustomContext("hmm_model", model_path);
    benchmark::AddCustomContext("user_dictionary", user_dict_path);
    benchmark::AddCustomContext("text", text_path);
    benchmark::AddCustomContext("lines", std::to_string(lines.size()));
    benchmark::AddCustomContext("runes", std::to_string(total_runes));
    benchmark::AddCustomContext("neo_load_ms", std::to_string(neo_load_ms));
    benchmark::AddCustomContext("old_load_ms", std::to_string(old_load_ms));

    // =====================================================================
    // Part 1: Correctness verification (Old == Neo for each method)
    // =====================================================================
    std::printf("Correctness: Old vs Neo\n");
    auto all_ok = true;
    all_ok &= verify_mode<CM::MIX>("MIX", old_jieba, jieba, lines);
    all_ok &= verify_mode<CM::MP>("MP", old_jieba, jieba, lines);
    all_ok &= verify_mode<CM::HMM>("HMM", old_jieba, jieba, lines);
    all_ok &= verify_mode<CM::FULL>("FULL", old_jieba, jieba, lines);
    all_ok &= verify_mode<CM::SEARCH>("SEARCH", old_jieba, jieba, lines);
    all_ok &= verify_method(
        "TAG", [&](const std::string &s) { return old_tagged_words(old_jieba, s); },
        [&](const std::string &s) { return neo_tagged_words(jieba, s); }, lines);
    std::printf("\n");
    if (!all_ok) {
        std::printf("WARNING: Some methods differ. Benchmark proceeds anyway.\n\n");
    }

    // =====================================================================
    // Part 2: Neo 5-method comparison (Unicode-level, core algorithm)
    // =====================================================================
    // Warm up: Google Benchmark's unreported iteration-count estimation runs precede every timed run.
    register_core(jieba, all_unicodes, total_runes);
    // ── Neo methods summary table ────────────────────────────────────────
    // ratio = time / fastest: compare the Core/<method> rows.

    // =====================================================================
    // Part 3: Old Jieba vs Neo Jieba (string-level, full pipeline)
    // =====================================================================
    register_string_method<CM::MIX>("MIX", old_jieba, jieba, lines, total_runes);
    register_string_method<CM::MP>("MP", old_jieba, jieba, lines, total_runes);
    register_string_method<CM::HMM>("HMM", old_jieba, jieba, lines, total_runes);
    register_string_method<CM::FULL>("FULL", old_jieba, jieba, lines, total_runes);
    register_string_method<CM::SEARCH>("SEARCH", old_jieba, jieba, lines, total_runes);
    // ── TAG ──
    register_string_pair(
        "TAG",
        [&old_jieba](const std::string &s) {
            auto w = std::vector<std::pair<std::string, std::string>>{};
            old_jieba.Tag(s, w);
            return w;
        },
        [&jieba](const std::string &s) { return jieba.cut_strings<neo_cppjieba::Tagging::Pos>(s, CM::MIX); }, lines,
        total_runes);
    // ── Old vs Neo summary table ─────────────────────────────────────────
    // speedup = Old time / Neo time: compare each String/<method>/old row with String/<method>/neo.

    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
