#pragma once

#include "neo/Config.hpp"
#include "neo/simd/SimdLevel.hpp" // IWYU pragma: keep (validates CPPJIEBA_SIMD_<LEVEL> before the #if below)
#include "neo/util/Logging.hpp"

#include <algorithm>
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
#elif defined(CPPJIEBA_SIMD_NEON)
#include <arm_neon.h>
#endif

namespace neo_cppjieba::detail::simd {

namespace scalar {

/// Append the index of every rune that satisfies is_match, in ascending order. Every backend must reproduce this
/// result; it also serves OFF builds unchanged. runes.size() must fit RuneIndex.
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

/// The block-mask kernel of one vector backend. Only the backends this build compiles specialize it, so naming
/// another one fails to compile.
template <SimdLevel Backend>
struct RuneMatchKernel;

// AVX2 builds compile this backend too, so tests compare both widths in one build.
#if defined(CPPJIEBA_SIMD_AVX2) || defined(CPPJIEBA_SIMD_SSE2)
template <>
struct RuneMatchKernel<SimdLevel::SSE2> {
    /// Return a mask whose bit k is set when block[k] equals one of needles, comparing 4 runes per step.
    template <std::size_t N>
    [[nodiscard]]
    static auto block_mask(std::span<const char32_t, match_block_size> block,
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
};
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
template <>
struct RuneMatchKernel<SimdLevel::AVX2> {
    /// Return a mask whose bit k is set when block[k] equals one of needles, comparing 8 runes per step.
    template <std::size_t N>
    [[nodiscard]]
    static auto block_mask(std::span<const char32_t, match_block_size> block,
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
};
#endif

#if defined(CPPJIEBA_SIMD_NEON)
template <>
struct RuneMatchKernel<SimdLevel::NEON> {
    /// Return a mask whose bit k is set when block[k] equals one of needles, comparing 8 runes narrowed to 16 bits per
    /// step. NEON has no movemask: one byte per rune is weighted, and three pairwise additions gather 8 runes per byte.
    template <std::size_t N>
    [[nodiscard]]
    static auto block_mask(std::span<const char32_t, match_block_size> block,
                           const std::array<char32_t, N> &needles) noexcept -> std::uint32_t {
        const auto *runes = block.data();
        const auto first = vmovn_high_u16(vmovn_u16(match_8_runes(runes, needles)), match_8_runes(runes + 8, needles));
        const auto second =
            vmovn_high_u16(vmovn_u16(match_8_runes(runes + 16, needles)), match_8_runes(runes + 24, needles));
        const auto weights = vld1q_u8(group_bits.data());
        auto sums = vpaddq_u8(vandq_u8(first, weights), vandq_u8(second, weights));
        sums = vpaddq_u8(sums, sums);
        sums = vpaddq_u8(sums, sums);
        // Bytes 0-3 now hold runes 0-7, 8-15, 16-23 and 24-31, so the little-endian lane is the block mask.
        return vgetq_lane_u32(vreinterpretq_u32_u8(sums), 0);
    }

private:
    /// Byte weights that fold 8 consecutive 0xFF/0x00 compare bytes into one mask byte under pairwise addition.
    static constexpr auto group_bits =
        std::array<std::uint8_t, 16>{1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};

    /// Return 0xFFFF in each 16-bit lane whose rune among runes[0, 8) equals one of needles. Saturating narrowing
    /// maps every rune above U+FFFF to 0xFFFF, which matches no needle because append_matches requires needles below
    /// it.
    template <std::size_t N>
    [[nodiscard]]
    static auto match_8_runes(const char32_t *runes, const std::array<char32_t, N> &needles) noexcept -> uint16x8_t {
        const auto low = vld1q_u32(reinterpret_cast<const std::uint32_t *>(runes));
        const auto high = vld1q_u32(reinterpret_cast<const std::uint32_t *>(runes + 4));
        const auto narrow = vqmovn_high_u32(vqmovn_u32(low), high);
        auto hits = vdupq_n_u16(0);
        for (const auto needle : needles) {
            hits = vorrq_u16(hits, vceqq_u16(narrow, vdupq_n_u16(static_cast<std::uint16_t>(needle))));
        }
        return hits;
    }
};
#endif

/// Append the index of every rune equal to one of needles, in ascending order, with Backend, one of
/// compiled_backends; callers pass `level` for the highest. is_match must accept exactly the needles: it classifies
/// block tails and every rune of the scalar backend. Every needle must be below U+FFFF, the value the NEON kernel
/// saturates wider runes to.
template <SimdLevel Backend, std::size_t N, typename IsMatch>
    requires std::predicate<const IsMatch &, char32_t>
inline auto append_matches(std::span<const char32_t> runes, const std::array<char32_t, N> &needles,
                           const IsMatch &is_match, std::vector<RuneIndex> &out) -> void {
    static_assert(is_compiled_backend(Backend), "simd::append_matches: this build does not compile the backend");
    assert_check([&] { return size_fits_capacity<RuneIndex>(runes.size()); },
                 "simd::append_matches: {} runes exceed the RuneIndex range", runes.size());
    assert_check(
        [&] { return std::ranges::all_of(needles, [](char32_t needle) { return needle < char32_t{0xFFFF}; }); },
        "simd::append_matches: every needle must be below U+FFFF");
    if constexpr (Backend == SimdLevel::OFF) {
        scalar::append_matches(runes, is_match, out);
    } else {
        const auto block_mask = [&](std::span<const char32_t, match_block_size> block) {
            return RuneMatchKernel<Backend>::block_mask(block, needles);
        };
        append_block_matches(runes, block_mask, is_match, out);
    }
}

} // namespace neo_cppjieba::detail::simd
