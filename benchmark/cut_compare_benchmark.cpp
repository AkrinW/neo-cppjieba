#include "BenchmarkUtils.hpp"
#include "CutCompareEngines.hpp"
#include "RustJiebaCapi.hpp"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <ios>
#include <locale>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr auto kLabels = std::array{
    "Old C++ strings",
    "Neo C++ strings",
    "Rust FFI copied -> C++ strings",
    "Rust FFI views -> C++ strings",
    "Rust native owned strings",
    "Rust native borrowed tokens",
    "Neo borrowed tokens",
    "Neo reused token positions",
};

constexpr auto kDisplayOrder = std::array<size_t, 8>{0, 1, 6, 7, 4, 5, 2, 3};

// Summarizes independent samples; each sample processes the complete corpus.
struct Timing {
    double median;
    double minimum;
    double maximum;
};

// Keep output differences visible independently of performance measurements.
struct Verification {
    size_t old_neo_mismatches = 0;
    size_t neo_rust_mismatches = 0;
    size_t old_tokens = 0;
    size_t neo_tokens = 0;
    size_t rust_tokens = 0;
};

// Stores all output representations for one segmentation mode.
struct MethodResult {
    const char *name;
    CompareMode mode;
    Verification verification;
    std::array<Timing, kLabels.size()> timings;
};

// Keep the standalone HMM measurements available to report consumers.
struct HmmResult {
    Timing old_strings;
    Timing neo_strings;
    size_t mismatches;
};

// Show a bounded output sample for a segmentation mismatch.
auto print_words_preview(const char *label, const std::vector<std::string> &words, size_t first_difference) -> void {
    std::printf("    %s:", label);
    const auto begin = first_difference > 3 ? first_difference - 3 : 0;
    for (auto i = begin; i < std::min(words.size(), first_difference + 5); ++i) {
        std::printf(" [%s]", words[i].c_str());
    }
    std::printf(" (%zu tokens)\n", words.size());
}

// Report a native Rust measurement in the measurement type the C++ engines share.
auto from_rust(const RustMeasurement &measurement) -> CutMeasurement {
    return {measurement.milliseconds, measurement.tokens};
}

// Verify both Rust adapters and report C++/Rust semantic differences separately.
template <typename RustFn, typename CopiedFn>
auto verify(const OldEngine &old, NeoEngine &neo, CompareMode mode, const RustFn &rust_fn, const CopiedFn &copied_fn,
            const std::vector<std::string> &lines) -> Verification {
    auto result = Verification{};
    auto previews = size_t{0};
    for (const auto &line : lines) {
        const auto old_words = old.cut(line, mode);
        const auto neo_words = neo.cut_strings(line, mode);
        neo.verify_outputs(line, mode, neo_words);
        const auto rust_words = rust_fn(line);
        if (rust_words != copied_fn(line)) {
            throw std::runtime_error("Rust copied and borrowed FFI results disagree");
        }
        result.old_tokens += old_words.size();
        result.neo_tokens += neo_words.size();
        result.rust_tokens += rust_words.size();
        result.old_neo_mismatches += old_words != neo_words;
        result.neo_rust_mismatches += neo_words != rust_words;
        if ((old_words != neo_words || neo_words != rust_words) && previews++ < 2) {
            const auto old_difference =
                std::mismatch(old_words.begin(), old_words.end(), neo_words.begin(), neo_words.end());
            const auto rust_difference =
                std::mismatch(neo_words.begin(), neo_words.end(), rust_words.begin(), rust_words.end());
            const auto first_difference = static_cast<size_t>(
                std::min(old_difference.first - old_words.begin(), rust_difference.first - neo_words.begin()));
            std::printf("  Mismatch near token %zu:\n", first_difference);
            print_words_preview("old ", old_words, first_difference);
            print_words_preview("neo ", neo_words, first_difference);
            print_words_preview("rust", rust_words, first_difference);
        }
    }
    std::printf("  Output mismatches: old/neo %zu/%zu, neo/rust %zu/%zu lines\n", result.old_neo_mismatches,
                lines.size(), result.neo_rust_mismatches, lines.size());
    return result;
}

// Report the median and full observed spread without selecting the fastest run.
auto summarize(std::vector<double> samples) -> Timing {
    std::sort(samples.begin(), samples.end());
    const auto middle = samples.size() / 2;
    const auto median = samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2 : samples[middle];
    return {median, samples.front(), samples.back()};
}

// Rotate execution order between samples, keeping warmup outside every measured sample.
auto run_shared_method(const char *name, RustCutMethod method, CompareMode mode, const OldEngine &old, NeoEngine &neo,
                       const RustJieba &rust, const std::vector<std::string> &lines, size_t rounds, size_t samples)
    -> MethodResult {
    std::printf("\n[%s]\n", name);
    const auto rust_fn = [&](const std::string &line) {
        return rust.cut(line, method, RustFfiOutput::Borrowed);
    };
    const auto copied_fn = [&](const std::string &line) {
        return rust.cut(line, method, RustFfiOutput::Copied);
    };
    const auto verification = verify(old, neo, mode, rust_fn, copied_fn, lines);
    const auto measure = [&](size_t variant, size_t sample_rounds) -> CutMeasurement {
        switch (variant) {
            case 0:
                return old.bench(lines, mode, sample_rounds);
            case 1:
                return neo.bench(lines, mode, NeoOutput::Strings, sample_rounds);
            case 2:
                return bench_cut(copied_fn, lines, sample_rounds);
            case 3:
                return bench_cut(rust_fn, lines, sample_rounds);
            case 4:
                return from_rust(rust.benchmark(lines, method, RustNativeOutput::Owned, sample_rounds));
            case 5:
                return from_rust(rust.benchmark(lines, method, RustNativeOutput::Borrowed, sample_rounds));
            case 6:
                return neo.bench(lines, mode, NeoOutput::Borrowed, sample_rounds);
            case 7:
                return neo.bench(lines, mode, NeoOutput::Reused, sample_rounds);
            default:
                break;
        }
        std::unreachable();
    };
    for (auto variant = size_t{0}; variant < kLabels.size(); ++variant) {
        DoNotOptimize(measure(variant, 1));
    }
    auto timings = std::array<std::vector<double>, kLabels.size()>{};
    for (auto &values : timings) {
        values.reserve(samples);
    }
    for (auto sample = size_t{0}; sample < samples; ++sample) {
        for (auto offset = size_t{0}; offset < kLabels.size(); ++offset) {
            const auto variant = (sample + offset) % kLabels.size();
            const auto result = measure(variant, rounds);
            const auto expected = variant == 0                   ? verification.old_tokens
                                  : variant == 1 || variant >= 6 ? verification.neo_tokens
                                                                 : verification.rust_tokens;
            if (result.tokens != expected * rounds) {
                throw std::runtime_error("Benchmark token count differs from correctness pass");
            }
            timings[variant].push_back(result.milliseconds);
        }
    }
    auto result = MethodResult{name, mode, verification, {}};
    std::printf("  %-36s %12s %12s %12s\n", "Path / output", "Median ms", "Min ms", "Max ms");
    for (auto variant = size_t{0}; variant < kLabels.size(); ++variant) {
        result.timings[variant] = summarize(std::move(timings[variant]));
    }
    for (const auto variant : kDisplayOrder) {
        const auto &t = result.timings[variant];
        std::printf("  %-36s %12.3f %12.3f %12.3f\n", kLabels[variant], t.median, t.minimum, t.maximum);
    }
    std::printf("  Removing the redundant Rust word copies: %.2fx (same C++ string output)\n",
                result.timings[2].median / result.timings[3].median);
    std::printf("  Neo strings/borrowed %.3fx; strings/reused %.3fx (same segmentation output)\n",
                result.timings[1].median / result.timings[6].median,
                result.timings[1].median / result.timings[7].median);
    if (verification.old_neo_mismatches != 0 || verification.neo_rust_mismatches != 0) {
        std::printf("  Cross-implementation ranking suppressed: segmentation outputs differ.\n");
    } else {
        std::printf("  Neo/Rust borrowed time: fresh %.3fx, reused %.3fx (lower is faster)\n",
                    result.timings[6].median / result.timings[5].median,
                    result.timings[7].median / result.timings[5].median);
    }
    return result;
}

// Preserve the standalone HMM comparison, which has no matching Rust public API.
auto run_hmm_pair(const OldEngine &old, NeoEngine &neo, const std::vector<std::string> &lines, size_t rounds,
                  size_t samples) -> HmmResult {
    auto mismatches = size_t{0};
    for (const auto &line : lines) {
        mismatches += old.cut(line, CompareMode::Hmm) != neo.cut_strings(line, CompareMode::Hmm);
    }
    const auto old_ms = [&] {
        return old.bench(lines, CompareMode::Hmm, rounds).milliseconds;
    };
    const auto neo_ms = [&] {
        return neo.bench(lines, CompareMode::Hmm, NeoOutput::Strings, rounds).milliseconds;
    };
    auto old_times = std::vector<double>{};
    auto neo_times = std::vector<double>{};
    old_times.reserve(samples);
    neo_times.reserve(samples);
    for (auto sample = size_t{0}; sample < samples; ++sample) {
        if (sample % 2 == 0) {
            old_times.push_back(old_ms());
            neo_times.push_back(neo_ms());
        } else {
            neo_times.push_back(neo_ms());
            old_times.push_back(old_ms());
        }
    }
    const auto result = HmmResult{summarize(std::move(old_times)), summarize(std::move(neo_times)), mismatches};
    std::printf("\n[HMM-only appendix] old %.3f ms, neo %.3f ms (medians); mismatches %zu/%zu lines\n",
                result.old_strings.median, result.neo_strings.median, result.mismatches, lines.size());
    return result;
}

// Reject malformed or zero iteration counts before loading engines or starting timers.
auto positive_count(std::string_view value) -> size_t {
    auto result = size_t{0};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0) {
        throw std::invalid_argument("rounds and samples must be positive integers");
    }
    return result;
}

// Serialize the same timing fields for shared modes and the standalone HMM pair.
auto write_timing(std::ostream &out, std::string_view label, const Timing &timing) -> void {
    out << "{\"label\":\"" << label << "\",\"median_ms\":" << timing.median << ",\"min_ms\":" << timing.minimum
        << ",\"max_ms\":" << timing.maximum << '}';
}

// All JSON labels are fixed ASCII literals; source text is represented only by numeric byte ranges.
auto write_report(const std::string &path, const std::array<MethodResult, 4> &results, const HmmResult &hmm,
                  NeoEngine &neo, const std::vector<std::string> &lines, size_t bytes, size_t rounds, size_t samples)
    -> void {
    auto out = std::ofstream{};
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(path);
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "{\"schema_version\":1,\"rounds\":" << rounds << ",\"samples\":" << samples
        << ",\"lines\":" << lines.size() << ",\"utf8_bytes\":" << bytes << ",\"methods\":[";
    for (auto method = size_t{0}; method < results.size(); ++method) {
        const auto &result = results[method];
        const auto &verification = result.verification;
        out << (method == 0 ? "{" : ",{") << "\"name\":\"" << result.name
            << "\",\"old_neo_mismatches\":" << verification.old_neo_mismatches
            << ",\"neo_rust_mismatches\":" << verification.neo_rust_mismatches
            << ",\"old_tokens\":" << verification.old_tokens << ",\"neo_tokens\":" << verification.neo_tokens
            << ",\"rust_tokens\":" << verification.rust_tokens << ",\"timings\":[";
        for (auto variant = size_t{0}; variant < kLabels.size(); ++variant) {
            if (variant != 0) {
                out << ',';
            }
            write_timing(out, kLabels[variant], result.timings[variant]);
        }
        out << "],\"neo_utf8_ranges\":";
        neo.write_ranges(out, result.mode, lines);
        out << '}';
    }
    out << "],\"hmm\":{\"old_neo_mismatches\":" << hmm.mismatches << ",\"timings\":[";
    write_timing(out, kLabels[0], hmm.old_strings);
    out << ',';
    write_timing(out, kLabels[1], hmm.neo_strings);
    out << "]}}\n";
    out.close();
}

// Run against a shared base dictionary and HMM file, excluding initialization from timing.
auto run(int argc, char *argv[]) -> int {
    const auto dict_path = argc > 1 ? std::string{argv[1]} : std::string{DICT_DIR} + "/jieba.dict.utf8";
    const auto model_path = argc > 2 ? std::string{argv[2]} : std::string{DICT_DIR} + "/hmm_model.utf8";
    const auto user_dict_path = argc > 3 ? std::string{argv[3]} : std::string{};
    const auto text_path = argc > 4 ? std::string{argv[4]} : std::string{TEST_DATA_DIR} + "/weicheng.utf8";
    const auto rounds = argc > 5 ? positive_count(argv[5]) : size_t{5};
    const auto samples = argc > 6 ? positive_count(argv[6]) : size_t{7};
    const auto report_path = argc > 7 ? std::string{argv[7]} : std::string{};
    if (argc > 8 || !user_dict_path.empty()) {
        throw std::invalid_argument(
            "Usage: cut_compare_benchmark [dict [model [\"\" [text [rounds [samples [report.json]]]]]]]; "
            "user dictionaries have different default-frequency semantics; use one shared base dictionary");
    }
    std::printf("jieba-rs 0.11.0, Cargo release opt-level=3; C++ %ld\n", static_cast<long>(__cplusplus));
    std::printf("Dictionary: %s\nHMM model (all engines): %s\nText: %s\n", dict_path.c_str(), model_path.c_str(),
                text_path.c_str());
    std::printf("No user dictionary; %zu rounds/sample, %zu rotated samples. Initialization and I/O excluded.\n",
                rounds, samples);
    std::printf("FFI copied reproduces the upstream C API copy pattern; FFI views removes that extra copy.\n");
    std::printf("Native owned returns Vec<String>; native borrowed returns tokens/offsets without string copies.\n");
    std::printf("Neo borrowed returns source text plus rune/source positions; reused retains decoding, segmentation, "
                "and output capacity.\n");
    std::printf("Reuse includes per-line clear/refill; final retained-buffer destruction is outside the timer.\n");
    std::printf("All Neo representations are checked against string output before timing.\n");
    const auto lines = load_lines(text_path);
    if (lines.empty()) {
        throw std::runtime_error("no input lines loaded from " + text_path);
    }
    auto bytes = size_t{0};
    for (const auto &line : lines) {
        bytes += line.size();
    }
    std::printf("Input: %zu nonempty lines, %zu UTF-8 bytes\n", lines.size(), bytes);
    // The legacy facade treats an empty path as its bundled user dictionary.
    const auto old = OldEngine{dict_path, model_path, std::string{TEST_DATA_DIR} + "/empty_user.dict.utf8"};
    auto neo = NeoEngine{dict_path, model_path};
    const auto rust = RustJieba{dict_path, model_path};
    const auto results = std::array{
        run_shared_method("MIX", RustCutMethod::Mix, CompareMode::Mix, old, neo, rust, lines, rounds, samples),
        run_shared_method("MP", RustCutMethod::Mp, CompareMode::Mp, old, neo, rust, lines, rounds, samples),
        run_shared_method("FULL", RustCutMethod::Full, CompareMode::Full, old, neo, rust, lines, rounds, samples),
        run_shared_method("SEARCH", RustCutMethod::Search, CompareMode::Search, old, neo, rust, lines, rounds, samples),
    };
    std::printf("\nMedian summary (ms; borrowed tokens have a different output contract)\n");
    std::printf("%-8s %10s %10s %10s %10s %10s %10s %10s %10s %12s\n", "Method", "Old", "Neo str", "Neo borrow",
                "Neo reuse", "RS owned", "RS borrow", "FFI copy", "FFI view", "NE/RS diffs");
    for (const auto &result : results) {
        std::printf("%-8s", result.name);
        for (const auto variant : kDisplayOrder) {
            std::printf(" %10.3f", result.timings[variant].median);
        }
        std::printf(" %8zu/%zu\n", result.verification.neo_rust_mismatches, lines.size());
    }
    const auto hmm = run_hmm_pair(old, neo, lines, rounds, samples);
    if (!report_path.empty()) {
        write_report(report_path, results, hmm, neo, lines, bytes, rounds, samples);
    }
    return 0;
}

} // namespace

// Convert benchmark input errors into a failing exit status with an actionable message.
auto main(int argc, char *argv[]) -> int {
    try {
        return run(argc, argv);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
