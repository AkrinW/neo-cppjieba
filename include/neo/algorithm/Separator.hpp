#pragma once

#include "neo/Config.hpp"
#include "neo/simd/RuneMatch.hpp"
#include "neo/simd/SimdLevel.hpp"
#include "neo/util/Logging.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

// ── Default separators ───────────────────────────────────────────────────────
// Equivalent to cppjieba::SPECIAL_SEPARATORS = " \t\n\xEF\xBC\x8C\xE3\x80\x82"
// which decodes to: space(U+0020), tab(U+0009), newline(U+000A), ，(U+FF0C), 。(U+3002).
// Sorted by code-point value for convenience.
inline constexpr auto DEFAULT_SEPARATORS = std::array<char32_t, 5>{
    U'\t',     // U+0009
    U'\n',     // U+000A
    U' ',      // U+0020
    U'\u3002', // 。
    U'\uFF0C', // ，
};

// is_default_separator checks whether a rune is one of the DEFAULT_SEPARATORS
// using a simple linear scan — optimal for a set this small (5 elements).
[[nodiscard]]
constexpr auto is_default_separator(char32_t r) noexcept -> bool {
    for (auto &&s : DEFAULT_SEPARATORS) {
        if (s == r) {
            return true;
        }
    }
    return false;
}

// is_default_separator_unlikely is a variant of is_default_separator that hints to the compiler that the condition is
// unlikely to be true.
[[nodiscard]]
constexpr auto is_default_separator_unlikely(char32_t r) noexcept -> bool {
    if (r <= 32) [[unlikely]] {
        // Bits set for tab(9), newline(10), space(32).
        constexpr auto bitmask = uint64_t{(1ULL << 9) | (1ULL << 10) | (1ULL << 32)};
        return (bitmask & (1ULL << r)) != 0;
    }
    return r == 0x3002 || r == 0xFF0C; // 。 or ，
}

// is_default_separator_branchless is a branchless variant of is_default_separator.
// It avoids if/else so the compiler can auto-vectorize loops that call it.
[[nodiscard]]
constexpr auto is_default_separator_branchless(char32_t r) noexcept -> bool {
    // low-range: tab(9), newline(10), space(32) — use bitmask
    auto low = (r <= 32) & (((uint64_t{(1ULL << 9) | (1ULL << 10) | (1ULL << 32)} >> (r & 63)) & 1) != 0);
    // high-range: 。(0x3002) or ，(0xFF0C)
    auto high = (r == 0x3002) | (r == 0xFF0C);
    return static_cast<bool>(low | high);
}

// get_pre_filter_separators scans the input runes and returns a vector of indices where DEFAULT_SEPARATORS occur.
// it is tested to be a speedup for pre-filtering when compared with iterator method, avoids repeated boundary searches
// and improves cache locality. produces reusable indices for downstream slicing without re-scanning.
inline auto get_pre_filter_separators(std::span<const char32_t> runes, std::vector<RuneIndex> &out) -> void {
    assert_check([&] { return detail::size_fits_capacity<RuneIndex>(runes.size()); },
                 "Separator input has {} runes, exceeding the supported index range", runes.size());
    auto n = static_cast<RuneIndex>(runes.size());
    out.clear();
    out.reserve(to_index(n / 8)); // heuristic: ~12.5% separators
    // A lambda keeps the per-rune predicate a direct call that the early inliner removes; a function reference
    // stays an indirect call until the translation-unit budget allows inlining append_matches.
    const auto is_separator = [](char32_t rune) noexcept {
        return is_default_separator_unlikely(rune);
    };
    simd::append_matches<simd::level>(runes, DEFAULT_SEPARATORS, is_separator, out);
    // The end sentinel lets callers slice [previous + 1, next) without special-casing the last segment.
    out.push_back(n);
}

// Convenience overload; segmenters use the output-parameter form to reuse workspace storage.
inline auto get_pre_filter_separators(std::span<const char32_t> runes) -> std::vector<RuneIndex> {
    auto out = std::vector<RuneIndex>{};
    get_pre_filter_separators(runes, out);
    return out;
}

} // namespace neo_cppjieba::detail
