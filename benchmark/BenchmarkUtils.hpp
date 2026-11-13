#pragma once

/// Shared utilities for benchmark programs:
///   - DoNotOptimize  : compiler barrier to prevent dead-code elimination
///   - Clock / Ms     : timing type aliases
///   - load_lines     : mmap-based UTF-8 line loader

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _MSC_VER
namespace benchmark_detail {
// Escape benchmark values before a compiler fence when GNU inline assembly is unavailable.
inline const void *volatile escaped_pointer = nullptr;
} // namespace benchmark_detail
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Anti-optimization barrier (like Google Benchmark's DoNotOptimize)
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
inline
auto DoNotOptimize(const T &value) -> void {
#ifdef _MSC_VER
    benchmark_detail::escaped_pointer = std::addressof(value);
    std::atomic_signal_fence(std::memory_order_seq_cst);
#else
    asm volatile("" : : "r,m"(value) : "memory");
#endif
}

template <typename T>
inline
auto DoNotOptimize(T &value) -> void {
#ifdef _MSC_VER
    benchmark_detail::escaped_pointer = std::addressof(value);
    std::atomic_signal_fence(std::memory_order_seq_cst);
#else
    asm volatile("" : "+m"(value) : : "memory");
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Timing helpers
// ─────────────────────────────────────────────────────────────────────────────

using Clock = std::chrono::high_resolution_clock;
using Ms = std::chrono::duration<double, std::milli>;

// ─────────────────────────────────────────────────────────────────────────────
// Load lines from a UTF-8 text file via mmap
// ─────────────────────────────────────────────────────────────────────────────

// Binary streams replace mmap for portable fixture loading outside the timed region.
inline
auto load_lines(const std::string &path) -> std::vector<std::string> {
    auto lines = std::vector<std::string>{};

    auto input = std::ifstream{std::filesystem::path{std::u8string{path.begin(), path.end()}}, std::ios::binary};
    if (!input.is_open()) {
        std::perror("open");
        return lines;
    }

    auto line = std::string{};
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    if (input.bad()) {
        std::perror("read");
        lines.clear();
    }
    return lines;
}
