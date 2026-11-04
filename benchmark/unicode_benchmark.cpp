/// Benchmark: Normal encode vs Fast encode (with source offsets)
///
/// Compares:
///   1. Decode overhead: decode() vs decode_with_source()
///   2. Full encode:     encode(span<Rune>) vs encode_range(0, n)
///   3. Sub-range encode: encode(subspan) vs encode_range(start, count)
///   4. Segmentation scenario: decode + cut into WordRanges + encode each word
///
/// Compile:
///   cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCPPJIEBA_BUILD_BENCHMARKS=ON
///   cmake --build build-bench --target unicode_benchmark
///
/// Run:
///   ./build-bench/benchmark/unicode_benchmark [--benchmark_<flag>...] [path/to/jieba.dict.utf8]

#include "neo/util/FileIO.hpp"
#include "neo/util/StringUtil.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"

#include "GoogleBenchmarkUtils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

namespace {

// Encode 100-rune chunks across the string
constexpr auto kChunkSize = std::size_t{100};

/// Build a large UTF-8 test string by concatenating all dictionary words.
auto build_test_string(const std::string &dict_path) -> std::string {
    auto file = read_file(dict_path);
    auto content = file.content();

    auto result = std::string{};
    result.reserve(content.size());

    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        if (line.empty()) {
            continue;
        }
        // Each line: "word freq [tag]" — take only the word (first field)
        auto split = get_split_view(line, ' ');
        for (auto &&field : split) {
            result.append(field);
            break; // only first field
        }
    }
    return result;
}

/// Simulate segmentation: generate realistic word ranges (avg ~2 runes per word)
/// that partition [0, total_runes) without gaps or overlaps.
auto simulate_segmentation(size_t total_runes, uint32_t seed = 42) -> std::vector<WordRange> {
    auto rng = std::mt19937{seed};
    // Word lengths: 1-4 runes, weighted towards 2 (typical Chinese segmentation)
    auto len_dist = std::discrete_distribution<uint32_t>{{15, 45, 30, 10}}; // P(1)=15%, P(2)=45%, P(3)=30%, P(4)=10%

    auto ranges = std::vector<WordRange>{};
    ranges.reserve(total_runes / 2);
    auto pos = uint32_t{0};
    auto limit = static_cast<uint32_t>(total_runes);
    while (pos < limit) {
        auto len = len_dist(rng) + 1; // 1..4
        if (pos + len > limit) {
            len = limit - pos;
        }
        ranges.push_back({pos, pos + len});
        pos += len;
    }
    return ranges;
}

// Number of chunks the sub-range benchmarks encode; a text shorter than one chunk is encoded as a single chunk.
auto chunk_count(std::size_t runes) -> std::size_t {
    return std::max(runes / kChunkSize, std::size_t{1});
}

// ─── 1. Decode benchmark ────────────────────────────────────────────
auto register_decode(const std::string &text) -> void {
    benchmark::RegisterBenchmark("Decode/normal", [&text](benchmark::State &state) {
        for (auto _ : state) {
            auto u = decode(text);
            benchmark::DoNotOptimize(u);
        }
        set_bytes_rate(state, text.size());
    });
    benchmark::RegisterBenchmark("Decode/sourced", [&text](benchmark::State &state) {
        for (auto _ : state) {
            auto ws = decode_with_offset(text);
            benchmark::DoNotOptimize(ws);
        }
        set_bytes_rate(state, text.size());
    });
}

// ─── 2. Full encode benchmark ───────────────────────────────────────
auto register_full_encode(const std::string &text, const Unicode &normal_decoded,
                          const UnicodeWithOffset &sourced_decoded) -> void {
    benchmark::RegisterBenchmark("FullEncode/normal", [&text, &normal_decoded](benchmark::State &state) {
        for (auto _ : state) {
            auto s = encode(std::span<const Rune>(normal_decoded));
            benchmark::DoNotOptimize(s);
        }
        set_bytes_rate(state, text.size());
    });
    benchmark::RegisterBenchmark("FullEncode/fast", [&text, &sourced_decoded](benchmark::State &state) {
        for (auto _ : state) {
            auto view = as_view(text);
            const auto &offsets = sourced_decoded.get_offsets();
            auto range = WordRange{0, static_cast<uint32_t>(sourced_decoded.get_runes().size())};
            auto s = encode(view, offsets, range);
            benchmark::DoNotOptimize(s);
        }
        set_bytes_rate(state, text.size());
    });
}

// ─── 3. Sub-range encode benchmark ──────────────────────────────────
auto register_subrange_encode(const std::string &text, const Unicode &normal_decoded,
                              const UnicodeWithOffset &sourced_decoded) -> void {
    benchmark::RegisterBenchmark("SubrangeEncode/normal", [&text, &normal_decoded](benchmark::State &state) {
        const auto num_chunks = chunk_count(normal_decoded.size());
        for (auto _ : state) {
            for (size_t c = 0; c < num_chunks; ++c) {
                auto offset = c * kChunkSize;
                auto count = std::min(kChunkSize, normal_decoded.size() - offset);
                auto s = encode(std::span<const Rune>(normal_decoded.data() + offset, count));
                benchmark::DoNotOptimize(s);
            }
        }
        set_bytes_rate(state, text.size());
    });
    benchmark::RegisterBenchmark("SubrangeEncode/fast", [&text, &sourced_decoded](benchmark::State &state) {
        const auto num_chunks = chunk_count(sourced_decoded.runes.size());
        for (auto _ : state) {
            for (size_t c = 0; c < num_chunks; ++c) {
                auto offset = c * kChunkSize;
                auto count = std::min(kChunkSize, sourced_decoded.runes.size() - offset);
                auto view = as_view(text);
                const auto &offsets = sourced_decoded.get_offsets();
                auto range = WordRange{static_cast<uint32_t>(offset), static_cast<uint32_t>(offset + count)};
                auto s = encode(view, offsets, range);
                benchmark::DoNotOptimize(s);
            }
        }
        set_bytes_rate(state, text.size());
    });
}

// ─── 4. Segmentation scenario ───────────────────────────────────────
// Per-word encode cost: items_per_second counts the encoded words. Both paths keep their rune and word buffers across
// iterations, as a reused workspace would; freeing them every iteration let glibc trim and refault the heap top.
auto register_segmentation(const std::string &text, const std::vector<WordRange> &ranges) -> void {
    // ─── Normal path: decode + encode(subspan) per word ─────────────────
    benchmark::RegisterBenchmark("Segmentation/normal", [&text, &ranges](benchmark::State &state) {
        auto runes = Unicode{};
        auto words = std::vector<std::string>{};
        words.reserve(ranges.size());
        for (auto _ : state) {
            // decode() is this call on a fresh buffer; no public entry point decodes into an existing Unicode.
            decode_into_impl<OffsetMode::Omit>(as_code_units(text), runes);
            words.clear();
            for (const auto &r : ranges) {
                words.push_back(encode(std::span<const Rune>(runes.data() + r.begin, r.size())));
            }
            benchmark::DoNotOptimize(words);
        }
        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(ranges.size()));
    });
    // ─── Fast path: decode_with_source + encode_range per word ──────────
    benchmark::RegisterBenchmark("Segmentation/fast", [&text, &ranges](benchmark::State &state) {
        auto ws = UnicodeWithOffset{};
        auto words = std::vector<std::string>{};
        words.reserve(ranges.size());
        for (auto _ : state) {
            decode_with_offset_into(text, ws);
            words.clear();
            auto view = as_view(text);
            const auto &offsets = ws.get_offsets();
            for (const auto &r : ranges) {
                words.push_back(encode(view, offsets, r));
            }
            benchmark::DoNotOptimize(words);
        }
        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(ranges.size()));
    });
}

// Build the test string and its decoded forms, then time each codec path.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 1, "Usage: unicode_benchmark [--benchmark_<flag>...] [path/to/jieba.dict.utf8]");
    const auto dict_path = positional_argument(args, 1, "dict/jieba.dict.utf8");
    const auto text = build_test_string(dict_path);
    // Pre-decode for encode benchmarks
    const auto normal_decoded = decode(text);
    const auto sourced_decoded = decode_with_offset(text);
    const auto ranges = simulate_segmentation(normal_decoded.size());
    benchmark::AddCustomContext("dictionary", dict_path);
    benchmark::AddCustomContext("test_string_bytes", std::to_string(text.size()));
    benchmark::AddCustomContext("test_string_runes", std::to_string(normal_decoded.size()));
    benchmark::AddCustomContext("simulated_words", std::to_string(ranges.size()));

    // Warm up: Google Benchmark's unreported iteration-count estimation runs precede every timed run.
    register_decode(text);
    register_full_encode(text, normal_decoded, sourced_decoded);
    register_subrange_encode(text, normal_decoded, sourced_decoded);
    register_segmentation(text, ranges);
    // Total roundtrip comparison: Decode/<path> plus FullEncode/<path> of the same path.
    benchmark::RunSpecifiedBenchmarks();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
