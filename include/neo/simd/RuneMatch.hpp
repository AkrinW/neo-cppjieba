#pragma once

#include "neo/Config.hpp"
#include "neo/simd/SimdLevel.hpp" // IWYU pragma: keep (validates CPPJIEBA_SIMD_<LEVEL> before the #if below)
#include "neo/util/Logging.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(CPPJIEBA_SIMD_AVX2)
#include <immintrin.h>
#elif defined(CPPJIEBA_SIMD_SSE2)
#include <emmintrin.h>
#endif

namespace neo_cppjieba::detail::simd {

namespace scalar {

/// Append the index of every rune that satisfies is_match, in ascending order. Every backend must reproduce this
/// result; it also serves OFF and NEON builds unchanged. runes.size() must fit RuneIndex.
template <typename IsMatch>
    requires std::predicate<const IsMatch &, char32_t>
inline auto append_matches(std::span<const char32_t> runes, const IsMatch &is_match, std::vector<RuneIndex> &out)
    -> void {
    auto n = static_cast<RuneIndex>(runes.size());
    for (auto i = RuneIndex{0}; i < n; ++i) {
        if (is_match(runes[to_index(i)])) {
            out.push_back(i);
        }
    }
}

} // namespace scalar

/// Runes each vector backend classifies per mask: one std::uint32_t bit per rune.
inline constexpr auto match_block_size = std::size_t{32};

/// Append the index of each set bit that block_mask reports for whole blocks, then classify the tail with is_match.
/// One mask per 32 runes keeps the loop-exit misprediction to about one per block instead of one per match.
template <typename BlockMask, typename IsMatch>
inline auto append_block_matches(std::span<const char32_t> runes, const BlockMask &block_mask, const IsMatch &is_match,
                                 std::vector<RuneIndex> &out) -> void {
    auto base = std::size_t{0};
    for (; runes.size() - base >= match_block_size; base += match_block_size) {
        const auto block = runes.subspan(base).first<match_block_size>();
        for (auto mask = std::uint32_t{block_mask(block)}; mask != 0; mask &= mask - 1) {
            out.push_back(static_cast<RuneIndex>(base + static_cast<std::size_t>(std::countr_zero(mask))));
        }
    }
    for (; base < runes.size(); ++base) {
        if (is_match(runes[base])) {
            out.push_back(static_cast<RuneIndex>(base));
        }
    }
}

#if defined(CPPJIEBA_SIMD_SSE2)
namespace sse2 {

/// Return a mask whose bit k is set when block[k] equals one of needles, comparing 4 runes per step.
template <std::size_t N>
[[nodiscard]] inline auto block_match_mask(std::span<const char32_t, match_block_size> block,
                                           const std::array<char32_t, N> &needles) noexcept -> std::uint32_t {
    constexpr auto lanes = std::size_t{4};
    auto mask = std::uint32_t{0};
    for (auto offset = std::size_t{0}; offset < match_block_size; offset += lanes) {
        const auto runes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(block.data() + offset));
        auto hits = _mm_setzero_si128();
        for (const auto needle : needles) {
            hits = _mm_or_si128(hits, _mm_cmpeq_epi32(runes, _mm_set1_epi32(static_cast<int>(needle))));
        }
        mask |= static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(hits))) << offset;
    }
    return mask;
}

} // namespace sse2
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
namespace avx2 {

/// Return a mask whose bit k is set when block[k] equals one of needles, comparing 8 runes per step.
template <std::size_t N>
[[nodiscard]] inline auto block_match_mask(std::span<const char32_t, match_block_size> block,
                                           const std::array<char32_t, N> &needles) noexcept -> std::uint32_t {
    constexpr auto lanes = std::size_t{8};
    auto mask = std::uint32_t{0};
    for (auto offset = std::size_t{0}; offset < match_block_size; offset += lanes) {
        const auto runes = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(block.data() + offset));
        auto hits = _mm256_setzero_si256();
        for (const auto needle : needles) {
            hits = _mm256_or_si256(hits, _mm256_cmpeq_epi32(runes, _mm256_set1_epi32(static_cast<int>(needle))));
        }
        mask |= static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(hits))) << offset;
    }
    return mask;
}

} // namespace avx2
#endif

/// Append the index of every rune equal to one of needles, in ascending order, with the highest backend the build
/// enables. is_match must accept exactly the needles: it classifies block tails and every rune of scalar builds.
template <std::size_t N, typename IsMatch>
    requires std::predicate<const IsMatch &, char32_t>
inline auto append_matches(std::span<const char32_t> runes, [[maybe_unused]] const std::array<char32_t, N> &needles,
                           const IsMatch &is_match, std::vector<RuneIndex> &out) -> void {
    assert_check([&] { return size_fits_capacity<RuneIndex>(runes.size()); },
                 "simd::append_matches: {} runes exceed the RuneIndex range", runes.size());
#if defined(CPPJIEBA_SIMD_AVX2)
    const auto block_mask = [&](std::span<const char32_t, match_block_size> block) {
        return avx2::block_match_mask(block, needles);
    };
    append_block_matches(runes, block_mask, is_match, out);
#elif defined(CPPJIEBA_SIMD_SSE2)
    const auto block_mask = [&](std::span<const char32_t, match_block_size> block) {
        return sse2::block_match_mask(block, needles);
    };
    append_block_matches(runes, block_mask, is_match, out);
#else
    scalar::append_matches(runes, is_match, out);
#endif
}

} // namespace neo_cppjieba::detail::simd
