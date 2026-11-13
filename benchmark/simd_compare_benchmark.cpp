// Times the Neo engine of cut_compare_benchmark alone with Google Benchmark, so builds at different CPPJIEBA_SIMD
// levels can be compared on one machine without the old CppJieba and Rust dependencies.
#include "neo/simd/SimdLevel.hpp"

#include "CutCompareEngines.hpp"
#include "GoogleBenchmarkUtils.hpp"
#include "test_paths.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <locale>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

namespace simd = neo_cppjieba::detail::simd;

constexpr auto kOutputs = std::array{NeoOutput::Strings, NeoOutput::Borrowed, NeoOutput::Reused};

// One measured segmentation mode with the name cut_compare_benchmark reports for it.
struct ModeSpec {
    const char *name;
    CompareMode mode;
};

constexpr auto kModes = std::array{
    ModeSpec{"MIX", CompareMode::Mix},       ModeSpec{"MP", CompareMode::Mp},   ModeSpec{"FULL", CompareMode::Full},
    ModeSpec{"SEARCH", CompareMode::Search}, ModeSpec{"HMM", CompareMode::Hmm},
};

// Name the level the whole program was built with, so reports from different builds stay distinguishable.
constexpr
auto simd_level_name(simd::SimdLevel level) -> const char * {
    switch (level) {
        case simd::SimdLevel::OFF:
            return "OFF";
        case simd::SimdLevel::SSE2:
            return "SSE2";
        case simd::SimdLevel::AVX2:
            return "AVX2";
        case simd::SimdLevel::NEON:
            return "NEON";
    }
    std::unreachable();
}

// Benchmark-name suffix of each Neo output, short enough for --benchmark_filter patterns.
constexpr
auto output_key(NeoOutput output) -> const char * {
    switch (output) {
        case NeoOutput::Strings:
            return "strings";
        case NeoOutput::Borrowed:
            return "borrowed";
        case NeoOutput::Reused:
            return "reused";
    }
    std::unreachable();
}

// FNV-1a over bytes: a compact fingerprint for checking that two builds produced identical token ranges.
auto fnv1a64(std::string_view bytes) -> std::uint64_t {
    return std::accumulate(bytes.begin(), bytes.end(), std::uint64_t{0xcbf29ce484222325},
                           [](std::uint64_t hash, char byte) {
                               return (hash ^ static_cast<unsigned char>(byte)) * std::uint64_t{0x100000001b3};
                           });
}

// Check every Neo output against the string output and return the corpus token count of one pass.
auto verify_mode(NeoEngine &neo, CompareMode mode, const std::vector<std::string> &lines) -> std::size_t {
    auto tokens = std::size_t{0};
    for (const auto &line : lines) {
        const auto words = neo.cut_strings(line, mode);
        neo.verify_outputs(line, mode, words);
        tokens += words.size();
    }
    return tokens;
}

// Fingerprint the UTF-8 token ranges of the corpus as 16 hex digits, outside every timer.
auto ranges_digest(NeoEngine &neo, CompareMode mode, const std::vector<std::string> &lines) -> std::string {
    auto ranges = std::ostringstream{};
    ranges.imbue(std::locale::classic());
    neo.write_ranges(ranges, mode, lines);
    return std::format("{:016x}", fnv1a64(ranges.view()));
}

// Time whole corpus passes with the engine's own timer, so the per-line loop stays in the Neo unit exactly as in
// cut_compare_benchmark. A pass whose token count differs from the correctness pass ends the benchmark with an error.
auto time_corpus_passes(benchmark::State &state, NeoEngine &neo, CompareMode mode, NeoOutput output,
                        const std::vector<std::string> &lines, std::size_t tokens, std::size_t bytes) -> void {
    for ([[maybe_unused]]
         auto _ : state) {
        const auto measurement = neo.bench(lines, mode, output, 1);
        state.SetIterationTime(measurement.milliseconds / 1000.0);
        if (measurement.tokens != tokens) {
            state.SkipWithError("token count differs from the correctness pass");
            return;
        }
    }
    set_bytes_rate(state, bytes);
}

// Verify every mode, record its token count and range digest in the report context, and register its outputs.
auto register_modes(NeoEngine &neo, const std::vector<std::string> &lines, std::size_t bytes) -> void {
    for (const auto &spec : kModes) {
        const auto tokens = verify_mode(neo, spec.mode, lines);
        benchmark::AddCustomContext(std::format("{}/tokens", spec.name), std::to_string(tokens));
        benchmark::AddCustomContext(std::format("{}/ranges_fnv1a64", spec.name), ranges_digest(neo, spec.mode, lines));
        for (const auto output : kOutputs) {
            benchmark::RegisterBenchmark(
                std::format("{}/{}", spec.name, output_key(output)),
                [&neo, &lines, mode = spec.mode, output, tokens, bytes](benchmark::State &state) {
                    time_corpus_passes(state, neo, mode, output, lines, tokens, bytes);
                })
                ->UseManualTime()
                ->Unit(benchmark::kMillisecond);
        }
    }
}

// Run every mode against one dictionary and HMM file; benchmark_main has already removed Google Benchmark's flags.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 3, "Usage: simd_compare_benchmark [--benchmark_<flag>...] [dict [model [text]]]");
    const auto dict_path = positional_argument(args, 1, std::string{DICT_DIR} + "/jieba.dict.utf8");
    const auto model_path = positional_argument(args, 2, std::string{DICT_DIR} + "/hmm_model.utf8");
    const auto text_path = positional_argument(args, 3, std::string{TEST_DATA_DIR} + "/weicheng.utf8");
    const auto lines = require_lines(text_path);
    const auto bytes = std::accumulate(lines.begin(), lines.end(), std::size_t{0},
                                       [](std::size_t total, const std::string &line) { return total + line.size(); });
    benchmark::AddCustomContext("simd_level", simd_level_name(simd::level));
    benchmark::AddCustomContext("text", text_path);
    benchmark::AddCustomContext("lines", std::to_string(lines.size()));
    benchmark::AddCustomContext("utf8_bytes", std::to_string(bytes));
    auto neo = NeoEngine{dict_path, model_path};
    register_modes(neo, lines, bytes);
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

// Convert benchmark input errors into a failing exit status with an actionable message.
auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
