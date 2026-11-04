#pragma once

/// Shared support for benchmark programs built on Google Benchmark:
///   - benchmark_main      : flag removal, input-error reporting and shutdown around a program's run function
///   - check_positional_count / positional_argument : the inputs left after Google Benchmark's flags
///   - require_lines       : a nonempty UTF-8 corpus
///   - set_corpus_rates    : line and rune throughput of iterations that each process a whole corpus
///   - set_bytes_rate      : byte throughput of iterations that each process the same input
///   - elapsed_seconds     : manual iteration time for work measured inside a larger iteration
///   - time_lookups / time_lookups_in_order / shuffled_indices : one lookup of every key per iteration
///   - time_corpus_cut     : one cut of every corpus line per iteration

#include "BenchmarkUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

/// Arguments left after benchmark::Initialize removed its --benchmark_ flags; index 0 is the program name.
using BenchmarkArgs = std::span<char *const>;

// Remove Google Benchmark's flags and call run with the remaining arguments. run prepares the inputs, registers the
// benchmarks and calls benchmark::RunSpecifiedBenchmarks while those inputs are alive; input errors exit with 1.
template <typename Run>
auto benchmark_main(int argc, char *argv[], const Run &run) -> int {
    benchmark::Initialize(&argc, argv);
    try {
        run(BenchmarkArgs{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    benchmark::Shutdown();
    return 0;
}

// Throw usage when more positional arguments remain than the program accepts.
inline auto check_positional_count(BenchmarkArgs args, std::size_t max_positional, const char *usage) -> void {
    if (args.size() > max_positional + 1) {
        throw std::invalid_argument(usage);
    }
}

// Return positional argument index (1 is the first after the program name), or fallback when it is absent.
inline auto positional_argument(BenchmarkArgs args, std::size_t index, const std::string &fallback) -> std::string {
    return index < args.size() ? std::string{args[index]} : fallback;
}

// Load the nonempty lines of a UTF-8 corpus, rejecting a missing or empty file before any benchmark runs.
inline auto require_lines(const std::string &path) -> std::vector<std::string> {
    auto lines = load_lines(path);
    if (lines.empty()) {
        throw std::runtime_error("no input lines loaded from " + path);
    }
    return lines;
}

// Report lines and runes per second for iterations that each process the whole corpus once.
inline auto set_corpus_rates(benchmark::State &state, std::size_t lines, std::size_t runes) -> void {
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(lines));
    state.counters["runes"] =
        benchmark::Counter(static_cast<double>(runes), benchmark::Counter::kIsIterationInvariantRate);
}

// Report bytes per second for iterations that each process the same bytes of input once.
inline auto set_bytes_rate(benchmark::State &state, std::size_t bytes) -> void {
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}

// Seconds since start, for benchmark::State::SetIterationTime when only part of an iteration is measured.
inline auto elapsed_seconds(Clock::time_point start) -> double {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Indices 0..count-1 in a fixed pseudo-random order, so shuffled lookups visit the same sequence in every run.
inline auto shuffled_indices(std::size_t count, std::uint32_t seed) -> std::vector<std::size_t> {
    auto shuffled = std::vector<std::size_t>(count);
    std::iota(shuffled.begin(), shuffled.end(), std::size_t{0});
    auto rng = std::mt19937{seed};
    std::ranges::shuffle(shuffled, rng);
    return shuffled;
}

// Cut every corpus line once per iteration; reports line and rune throughput over the given rune total.
template <typename CutFn, typename Line>
auto time_corpus_cut(benchmark::State &state, const CutFn &cut, const std::vector<Line> &lines, std::size_t runes)
    -> void {
    for (auto _ : state) {
        for (const auto &line : lines) {
            auto result = cut(line);
            benchmark::DoNotOptimize(result);
        }
    }
    set_corpus_rates(state, lines.size(), runes);
}

// Look up every key once per iteration in stored order; items_per_second counts lookups.
template <typename FindFn, typename Key>
auto time_lookups(benchmark::State &state, const FindFn &find, const std::vector<Key> &keys) -> void {
    for (auto _ : state) {
        for (const auto &key : keys) {
            auto result = find(key);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}

// Look up every key once per iteration in the given visiting order; items_per_second counts lookups.
template <typename FindFn, typename Key>
auto time_lookups_in_order(benchmark::State &state, const FindFn &find, const std::vector<Key> &keys,
                           const std::vector<std::size_t> &order) -> void {
    for (auto _ : state) {
        for (const auto index : order) {
            auto result = find(keys[index]);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(order.size()));
}
