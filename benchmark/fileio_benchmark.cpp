#include "limonp/StringUtil.hpp"
#include "neo/util/FileIO.hpp"
#include "neo/util/StringUtil.hpp"

#include "test_paths.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

// ─── Performance comparison: old (ifstream+getline) vs new (read+views) ──────

// Helper: high-resolution timer
using hrc = std::chrono::high_resolution_clock;

// Old method: ifstream + getline + limonp::Split — mimics DictTrie::LoadDict
namespace {
static auto BenchOldMethod(const std::string &path, int iterations) -> std::chrono::microseconds {
    auto total = std::chrono::microseconds::zero();

    for (int i = 0; i < iterations; ++i) {
        auto start = hrc::now();

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
                [[maybe_unused]] volatile auto *w = buf[0].data();
                // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): mirrors CppJieba's own parsing.
                [[maybe_unused]] volatile auto f = std::atof(buf[1].c_str());
                [[maybe_unused]] volatile auto *t = buf[2].data();
            }
            ++line_count;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(hrc::now() - start);
        total += elapsed;
        if (line_count <= 300000) {
            throw std::runtime_error("benchmark requires the full jieba dictionary: " + path);
        }
    }
    return total / iterations;
}

// New method: FileBuffer + lines_view + split_view — parsing without further copies
static auto BenchNewMethod(const std::string &path, int iterations) -> std::chrono::microseconds {
    auto total = std::chrono::microseconds::zero();

    for (int i = 0; i < iterations; ++i) {
        auto start = hrc::now();

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
                        [[maybe_unused]] const volatile auto *w = field.data();
                        break;
                    }
                    case 1: {
                        // std::from_chars avoids requiring null-terminated string
                        auto val = 0.0;
                        std::from_chars(field.data(), field.data() + field.size(), val);
                        [[maybe_unused]] const volatile auto f = val;
                        break;
                    }
                    case 2: {
                        [[maybe_unused]] const volatile auto *t = field.data();
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

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(hrc::now() - start);
        total += elapsed;
        if (line_count <= 300000) {
            throw std::runtime_error("benchmark requires the full jieba dictionary: " + path);
        }
    }
    return total / iterations;
}

// Compare full-dictionary loading after warming the filesystem cache.
auto run_benchmark() -> void {
    auto path = std::string(DICT_DIR) + "/jieba.dict.utf8";
    constexpr auto ITERATIONS = 5;

    // Warm up filesystem cache
    {
        auto _ = read_file(path);
    }
    {
        auto ifs = std::ifstream{path.c_str()};
        auto line = std::string{};
        while (std::getline(ifs, line)) {
        }
    }

    auto old_avg = BenchOldMethod(path, ITERATIONS);
    auto new_avg = BenchNewMethod(path, ITERATIONS);

    auto speedup = static_cast<double>(old_avg.count()) / static_cast<double>(new_avg.count());

    std::fprintf(stderr,
                 "\n"
                 "  ┌───────────────────────────────────────────────────┐\n"
                 "  │  FileIO Performance (jieba.dict.utf8, 349K lines) │\n"
                 "  ├───────────────────────────────────────────────────┤\n"
                 "  │  Old (ifstream+getline+Split) : %6ld µs           │\n"
                 "  │  New (read+lines_view+split)  : %6ld µs           │\n"
                 "  │  Speedup                      : %5.2fx            │\n"
                 "  └───────────────────────────────────────────────────┘\n\n",
                 static_cast<long>(old_avg.count()), static_cast<long>(new_avg.count()), speedup);

    // The new method should be faster (at least not slower)
    // Report this expectation without making timing a pass/fail condition.
}
} // namespace

// Report invalid benchmark input as a failing command with a readable diagnostic.
auto main() -> int {
    try {
        run_benchmark();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
