#pragma once
/// Engines compared by cut_compare_benchmark. Old CppJieba and Neo are each defined in their own translation unit,
/// so the compiler inlines each library as in an application that includes only that library; the per-line work and
/// its timing loop stay inside that unit, and the driver crosses units once per timed corpus pass.

#include "BenchmarkUtils.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

/// Segmentation modes the benchmark compares; each maps to one old CppJieba call and one Neo CutMode.
enum class CompareMode : std::uint8_t { Mix, Mp, Full, Search, Hmm };

/// Neo output representations that are timed separately.
enum class NeoOutput : std::uint8_t { Strings, Borrowed, Reused };

/// Elapsed time and produced token count of one timed loop over the corpus.
struct CutMeasurement {
    double milliseconds;
    std::size_t tokens;
};

// Include output allocation and destruction in the C++ end-to-end timer.
// Reference results keep caller-owned buffers alive; decltype(auto) prevents a benchmark-only copy.
template <typename CutFn>
auto bench_cut(const CutFn &fn, const std::vector<std::string> &lines, std::size_t rounds) -> CutMeasurement {
    auto tokens = std::size_t{0};
    const auto start = std::chrono::steady_clock::now();
    for (auto round = std::size_t{0}; round < rounds; ++round) {
        for (const auto &line : lines) {
            DoNotOptimize(line);
            decltype(auto) words = fn(line);
            tokens += words.size();
            DoNotOptimize(words);
        }
    }
    return {Ms(std::chrono::steady_clock::now() - start).count(), tokens};
}

/// Old CppJieba with its string-vector outputs; defined in cut_compare_old.cpp.
class OldEngine {
public:
    OldEngine(const std::string &dict_path, const std::string &model_path, const std::string &user_dict_path);
    ~OldEngine();
    OldEngine(const OldEngine &) = delete;
    auto operator=(const OldEngine &) -> OldEngine & = delete;

    [[nodiscard]]
    auto cut(const std::string &line, CompareMode mode) const -> std::vector<std::string>;
    [[nodiscard]]
    auto bench(const std::vector<std::string> &lines, CompareMode mode, std::size_t rounds) const -> CutMeasurement;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Neo with its string, borrowed and reused outputs; defined in cut_compare_neo.cpp.
/// The reused path keeps one workspace and position buffer for the engine's lifetime, outside every timer.
class NeoEngine {
public:
    NeoEngine(const std::string &dict_path, const std::string &model_path);
    ~NeoEngine();
    NeoEngine(const NeoEngine &) = delete;
    auto operator=(const NeoEngine &) -> NeoEngine & = delete;

    [[nodiscard]]
    auto cut_strings(const std::string &line, CompareMode mode) const -> std::vector<std::string>;
    // Throw unless the borrowed and reused outputs of line spell exactly the expected string tokens.
    auto verify_outputs(const std::string &line, CompareMode mode, const std::vector<std::string> &expected) -> void;
    [[nodiscard]]
    auto bench(const std::vector<std::string> &lines, CompareMode mode, NeoOutput output, std::size_t rounds)
        -> CutMeasurement;
    // Write the UTF-8 byte range of every token as a JSON array of per-line [begin, end] pairs.
    auto write_ranges(std::ostream &out, CompareMode mode, const std::vector<std::string> &lines) -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
