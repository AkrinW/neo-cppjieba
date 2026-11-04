#include "limonp/StringUtil.hpp"
#include "neo/util/FileIO.hpp"
#include "neo/util/StringUtil.hpp"

#include "GoogleBenchmarkUtils.hpp"
#include "test_paths.h"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

// ─── Performance comparison: old (ifstream+getline) vs new (read+views) ──────

// Helper: high-resolution timer — Google Benchmark's timer now measures every parse.

namespace {

// Smallest line count that identifies the full jieba dictionary the comparison is meant for.
constexpr auto kFullDictionaryLines = size_t{300000};

// Old method: ifstream + getline + limonp::Split — mimics DictTrie::LoadDict
auto parse_old_method(const std::string &path) -> size_t {
    auto ifs = std::ifstream{path.c_str()};
    if (!ifs.is_open()) {
        throw std::runtime_error("cannot open dictionary: " + path);
    }
    auto line = std::string{};
    auto buf = std::vector<std::string>{};

    auto line_count = size_t{0};
    while (std::getline(ifs, line)) {
        limonp::Split(line, buf, " ");
        // Simulate the same work DictTrie::LoadDict does: access all 3 fields
        if (buf.size() == 3) {
            [[maybe_unused]]
            volatile auto *w = buf[0].data();
            // NOLINTBEGIN(bugprone-unchecked-string-to-number-conversion): mirrors CppJieba's own parsing.
            [[maybe_unused]]
            volatile auto f = std::atof(buf[1].c_str());
            // NOLINTEND(bugprone-unchecked-string-to-number-conversion)
            [[maybe_unused]]
            volatile auto *t = buf[2].data();
        }
        ++line_count;
    }
    return line_count;
}

// New method: FileBuffer + lines_view + split_view — parsing without further copies
auto parse_new_method(const std::string &path) -> size_t {
    auto file = read_file(path);
    auto content = file.content();

    auto line_count = size_t{0};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        if (line.empty()) {
            continue;
        }
        auto field_idx = size_t{0};
        auto split = get_split_view(line, ' ');
        for (auto &&field : split) {
            // Simulate the same work: access each field
            switch (field_idx) {
                case 0: {
                    [[maybe_unused]]
                    const volatile auto *w = field.data();
                    break;
                }
                case 1: {
                    // std::from_chars avoids requiring null-terminated string
                    auto val = 0.0;
                    std::from_chars(field.data(), field.data() + field.size(), val);
                    [[maybe_unused]]
                    const volatile auto f = val;
                    break;
                }
                case 2: {
                    [[maybe_unused]]
                    const volatile auto *t = field.data();
                    break;
                }
                default: {
                    break;
                }
            }
            ++field_idx;
        }
        ++line_count;
    }
    return line_count;
}

// Time one parsing method per iteration, rejecting a dictionary smaller than the full jieba one.
template <typename ParseFn>
auto time_parse(benchmark::State &state, const ParseFn &parse, const std::string &path) -> void {
    for (auto _ : state) {
        auto line_count = parse(path);
        if (line_count <= kFullDictionaryLines) {
            state.SkipWithError("benchmark requires the full jieba dictionary: " + path);
            return;
        }
        benchmark::DoNotOptimize(line_count);
    }
}

// Compare full-dictionary loading after warming the filesystem cache.
auto run(BenchmarkArgs args) -> void {
    check_positional_count(args, 0, "Usage: fileio_benchmark [--benchmark_<flag>...]");
    const auto path = std::string(DICT_DIR) + "/jieba.dict.utf8";

    // Warm up filesystem cache
    if (parse_new_method(path) <= kFullDictionaryLines || parse_old_method(path) <= kFullDictionaryLines) {
        throw std::runtime_error("benchmark requires the full jieba dictionary: " + path);
    }
    benchmark::AddCustomContext("dictionary", path);

    // Old (ifstream+getline+Split)
    benchmark::RegisterBenchmark("DictParse/old_getline_split", [&path](benchmark::State &state) {
        time_parse(state, parse_old_method, path);
    })->Unit(benchmark::kMillisecond);
    // New (read+lines_view+split)
    benchmark::RegisterBenchmark("DictParse/new_views", [&path](benchmark::State &state) {
        time_parse(state, parse_new_method, path);
    })->Unit(benchmark::kMillisecond);

    // The new method should be faster (at least not slower)
    // Report this expectation without making timing a pass/fail condition.
    benchmark::RunSpecifiedBenchmarks();
}
} // namespace

// Report invalid benchmark input as a failing command with a readable diagnostic.
auto main(int argc, char *argv[]) -> int {
    return benchmark_main(argc, argv, run);
}
