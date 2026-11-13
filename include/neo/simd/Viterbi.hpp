#pragma once

#include "neo/simd/SimdLevel.hpp" // IWYU pragma: keep (validates CPPJIEBA_SIMD_<LEVEL> before the #if below)
#include "neo/util/Logging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#if defined(CPPJIEBA_SIMD_AVX2) || defined(CPPJIEBA_SIMD_SSE2) || defined(CPPJIEBA_SIMD_NEON)
#include "neo/util/ForceInline.hpp"

#include <bit>
#include <cstdint>
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
#include <immintrin.h>
#elif defined(CPPJIEBA_SIMD_SSE2)
#include <emmintrin.h>
#elif defined(CPPJIEBA_SIMD_NEON)
#include <arm_neon.h>
#endif

// Forward-pass kernels for the Viterbi in algorithm/HMMSegment.hpp. Each column scores the states B, E, M, S. Under
// the CppJieba rule a state keeps the first predecessor whose (weight + transition) + emission strictly exceeds the
// floor and every earlier candidate; under the Python/Rust rule it weighs only its two legal predecessors and keeps
// the later one on a tie. Vector backends add in the same order and break ties the same way, so they match the scalar
// loops bit for bit.

namespace neo_cppjieba::detail::simd {

/// Scores of the four HMM states in B, E, M, S order.
using StateScores = std::array<double, 4>;

/// The score a candidate must exceed, and the predecessor a state keeps when no candidate exceeds it.
template <typename State>
struct ViterbiFloor {
    double score;
    State state;
};

/// One-byte states numbered 0–3 in B, E, M, S order, and emit_at(x) returning the emissions of column x.
template <typename State, typename EmitAt>
concept ViterbiInputs =
    std::is_enum_v<State> && sizeof(State) == 1 && std::is_invocable_r_v<StateScores, const EmitAt &, std::size_t>;

/// The two legal predecessors of each state under the Python/Rust rule, earlier first: B and S follow E or S, and
/// E and M follow B or M.
inline constexpr auto legal_predecessors = std::array<std::array<std::size_t, 2>, 4>{{{1, 3}, {0, 2}, {0, 2}, {1, 3}}};

// Whether every score is finite; vector backends check each column in debug builds, as the scalar loop checks
// each candidate.
inline
auto finite_scores(const StateScores &scores) noexcept -> bool {
    return std::ranges::all_of(scores, [](double score) { return std::isfinite(score); });
}

namespace scalar {

/// Score columns 1 .. path.size() - 1 from the column-0 weights; path[x][y] receives the predecessor of state y at
/// column x, and the result holds the last column's weights. Every backend must reproduce this result exactly.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline
auto viterbi_forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
                     const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
    for (auto x = std::size_t{1}; x < path.size(); ++x) {
        const auto emit_probs = emit_at(x);
        auto current = StateScores{};
        for (auto y = std::size_t{0}; y < current.size(); ++y) {
            const auto emit = emit_probs[y];
            auto best_weight = floor.score;
            auto best_prev = floor.state;
            for (auto prev_y = std::size_t{0}; prev_y < weights.size(); ++prev_y) {
                const auto w = weights[prev_y] + transitions[prev_y][y] + emit;
                assert_check([&] { return std::isfinite(w); }, "HMMSegment: non-finite accumulated weight");
                if (w > best_weight) {
                    best_weight = w;
                    best_prev = static_cast<State>(prev_y);
                }
            }
            assert_check([&] { return std::to_underlying(best_prev) < 4 && std::isfinite(best_weight); },
                         "HMMSegment: Viterbi must select a valid predecessor");
            current[y] = best_weight;
            path[x][y] = best_prev;
        }
        weights = current;
    }
    return weights;
}

/// Same contract as viterbi_forward with no floor: each state weighs only its legal predecessors and keeps the later
/// one when their scores tie. Every backend must reproduce this result exactly.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline
auto viterbi_forward_legal(StateScores weights, std::span<const StateScores, 4> transitions, const EmitAt &emit_at,
                           std::span<std::array<State, 4>> path) -> StateScores {
    for (auto x = std::size_t{1}; x < path.size(); ++x) {
        const auto emit_probs = emit_at(x);
        auto current = StateScores{};
        for (auto y = std::size_t{0}; y < current.size(); ++y) {
            const auto [earlier, later] = legal_predecessors[y];
            const auto earlier_weight = weights[earlier] + transitions[earlier][y] + emit_probs[y];
            const auto later_weight = weights[later] + transitions[later][y] + emit_probs[y];
            const auto earlier_wins = earlier_weight > later_weight;
            current[y] = earlier_wins ? earlier_weight : later_weight;
            path[x][y] = static_cast<State>(earlier_wins ? earlier : later);
            assert_check([&] { return std::isfinite(current[y]); }, "HMMSegment: non-finite accumulated weight");
        }
        weights = current;
    }
    return weights;
}

} // namespace scalar

/// The forward-pass kernels of one vector backend: forward follows the CppJieba rule and forward_legal the
/// Python/Rust rule. Only the backends this build compiles specialize it, so naming another one fails to compile.
template <SimdLevel Backend>
struct ViterbiKernel;

#if defined(CPPJIEBA_SIMD_AVX2) || defined(CPPJIEBA_SIMD_SSE2) || defined(CPPJIEBA_SIMD_NEON)
// Lane y holds the transition into state y from its earlier (slot 0) or later (slot 1) legal predecessor.
inline
auto legal_transitions(std::span<const StateScores, 4> transitions, std::size_t slot) noexcept -> StateScores {
    auto rows = StateScores{};
    for (auto y = std::size_t{0}; y < rows.size(); ++y) {
        rows[y] = transitions[legal_predecessors[y][slot]][y];
    }
    return rows;
}

// The earlier (slot 0) or later (slot 1) legal predecessor of every state, one byte per state.
constexpr
auto legal_predecessor_bytes(std::size_t slot) noexcept -> std::uint32_t {
    auto bytes = std::uint32_t{0};
    for (auto y = std::size_t{0}; y < legal_predecessors.size(); ++y) {
        bytes |= static_cast<std::uint32_t>(legal_predecessors[y][slot]) << (8 * y);
    }
    return bytes;
}
#endif

#if defined(CPPJIEBA_SIMD_AVX2) || defined(CPPJIEBA_SIMD_SSE2)

// Move bit y of a four-bit lane mask to the low bit of byte y; the four shifted copies of the mask never overlap.
[[nodiscard, NEO_FORCE_INLINE]] constexpr
auto spread_lanes(std::uint32_t mask) noexcept -> std::uint32_t {
    return (mask * std::uint32_t{0x00204081}) & std::uint32_t{0x01010101};
}

// Byte y receives the earlier legal predecessor of state y where earlier_wins has bit y, else the later one.
[[nodiscard, NEO_FORCE_INLINE]] constexpr
auto pack_legal(std::uint32_t earlier_wins) noexcept -> std::uint32_t {
    constexpr auto later_bytes = legal_predecessor_bytes(1);
    // Every legal pair is two states apart, so one subtraction per byte picks the earlier state.
    static_assert(later_bytes - legal_predecessor_bytes(0) == 0x02020202U);
    return later_bytes - 2 * spread_lanes(earlier_wins);
}

// AVX2 builds compile this backend too, so tests and measurements can compare both widths in one build.
template <>
struct ViterbiKernel<SimdLevel::SSE2> {
    /// Same contract as scalar::viterbi_forward. Each column is two halves, (B, E) and (M, S), of two doubles each.
    template <typename State, typename EmitAt>
        requires ViterbiInputs<State, EmitAt>
    static
    auto forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
                 const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
        const auto rows_low = HalfRows{.from_b = _mm_loadu_pd(transitions[0].data()),
                                       .from_e = _mm_loadu_pd(transitions[1].data()),
                                       .from_m = _mm_loadu_pd(transitions[2].data()),
                                       .from_s = _mm_loadu_pd(transitions[3].data())};
        const auto rows_high = HalfRows{.from_b = _mm_loadu_pd(transitions[0].data() + 2),
                                        .from_e = _mm_loadu_pd(transitions[1].data() + 2),
                                        .from_m = _mm_loadu_pd(transitions[2].data() + 2),
                                        .from_s = _mm_loadu_pd(transitions[3].data() + 2)};
        const auto floor_score = _mm_set1_pd(floor.score);
        const auto floor_state = _mm_set1_pd(static_cast<double>(std::to_underlying(floor.state)));
        auto low = _mm_loadu_pd(weights.data());
        auto high = _mm_loadu_pd(weights.data() + 2);
        for (auto x = std::size_t{1}; x < path.size(); ++x) {
            const auto emit_probs = emit_at(x);
            const auto broadcasts = HalfRows{.from_b = _mm_unpacklo_pd(low, low),
                                             .from_e = _mm_unpackhi_pd(low, low),
                                             .from_m = _mm_unpacklo_pd(high, high),
                                             .from_s = _mm_unpackhi_pd(high, high)};
            const auto first =
                score_half(broadcasts, rows_low, _mm_loadu_pd(emit_probs.data()), floor_score, floor_state);
            const auto second =
                score_half(broadcasts, rows_high, _mm_loadu_pd(emit_probs.data() + 2), floor_score, floor_state);
            path[x] = std::bit_cast<std::array<State, 4>>(pack_states(first.prev, second.prev));
            low = _mm_max_pd(first.best, floor_score);
            high = _mm_max_pd(second.best, floor_score);
            assert_check(
                [&] {
                    auto scores = StateScores{};
                    _mm_storeu_pd(scores.data(), low);
                    _mm_storeu_pd(scores.data() + 2, high);
                    return finite_scores(scores);
                },
                "HMMSegment: non-finite accumulated weight");
        }
        _mm_storeu_pd(weights.data(), low);
        _mm_storeu_pd(weights.data() + 2, high);
        return weights;
    }

    /// Same contract as scalar::viterbi_forward_legal. States (B, E) weigh predecessors (E, B) against (S, M), and
    /// states (M, S) weigh (B, E) against (M, S).
    template <typename State, typename EmitAt>
        requires ViterbiInputs<State, EmitAt>
    static
    auto forward_legal(StateScores weights, std::span<const StateScores, 4> transitions, const EmitAt &emit_at,
                       std::span<std::array<State, 4>> path) -> StateScores {
        const auto earlier_rows = legal_transitions(transitions, 0);
        const auto later_rows = legal_transitions(transitions, 1);
        const auto earlier_low = _mm_loadu_pd(earlier_rows.data());
        const auto earlier_high = _mm_loadu_pd(earlier_rows.data() + 2);
        const auto later_low = _mm_loadu_pd(later_rows.data());
        const auto later_high = _mm_loadu_pd(later_rows.data() + 2);
        auto low = _mm_loadu_pd(weights.data());
        auto high = _mm_loadu_pd(weights.data() + 2);
        for (auto x = std::size_t{1}; x < path.size(); ++x) {
            const auto emit_probs = emit_at(x);
            const auto emit_low = _mm_loadu_pd(emit_probs.data());
            const auto emit_high = _mm_loadu_pd(emit_probs.data() + 2);
            const auto low_earlier = _mm_add_pd(_mm_add_pd(swap_lanes(low), earlier_low), emit_low);
            const auto low_later = _mm_add_pd(_mm_add_pd(swap_lanes(high), later_low), emit_low);
            const auto high_earlier = _mm_add_pd(_mm_add_pd(low, earlier_high), emit_high);
            const auto high_later = _mm_add_pd(_mm_add_pd(high, later_high), emit_high);
            // _mm_max_pd(a, b) returns a only when a > b, so a tie keeps the later predecessor as the scalar loop does.
            low = _mm_max_pd(low_earlier, low_later);
            high = _mm_max_pd(high_earlier, high_later);
            const auto earlier_wins = lane_bits(_mm_cmpgt_pd(low_earlier, low_later))
                                      | (lane_bits(_mm_cmpgt_pd(high_earlier, high_later)) << 2);
            path[x] = std::bit_cast<std::array<State, 4>>(pack_legal(earlier_wins));
            assert_check(
                [&] {
                    auto scores = StateScores{};
                    _mm_storeu_pd(scores.data(), low);
                    _mm_storeu_pd(scores.data() + 2, high);
                    return finite_scores(scores);
                },
                "HMMSegment: non-finite accumulated weight");
        }
        _mm_storeu_pd(weights.data(), low);
        _mm_storeu_pd(weights.data() + 2, high);
        return weights;
    }

private:
    /// One value per predecessor state for two target states; a named struct, since std::array drops vector attributes.
    struct HalfRows {
        __m128d from_b;
        __m128d from_e;
        __m128d from_m;
        __m128d from_s;
    };

    /// The best score of two states and the index (as a double) of its first predecessor, or the floor state.
    struct HalfColumn {
        __m128d best;
        __m128d prev;
    };

    // Pick a where mask is set and b elsewhere.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto select(__m128d mask, __m128d a, __m128d b) noexcept -> __m128d {
        return _mm_or_pd(_mm_and_pd(mask, a), _mm_andnot_pd(mask, b));
    }

    // Score two states from the four predecessor weights, each broadcast to both lanes. _mm_max_pd(a, b) returns b
    // unless a > b, so max(later, earlier) keeps the earlier candidate on a tie, as the scalar loop does.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto score_half(const HalfRows &weights, const HalfRows &rows, __m128d emit, __m128d floor_score,
                    __m128d floor_state) noexcept -> HalfColumn {
        const auto c0 = _mm_add_pd(_mm_add_pd(weights.from_b, rows.from_b), emit);
        const auto c1 = _mm_add_pd(_mm_add_pd(weights.from_e, rows.from_e), emit);
        const auto c2 = _mm_add_pd(_mm_add_pd(weights.from_m, rows.from_m), emit);
        const auto c3 = _mm_add_pd(_mm_add_pd(weights.from_s, rows.from_s), emit);
        const auto best = _mm_max_pd(_mm_max_pd(c3, c2), _mm_max_pd(c1, c0));
        auto prev = _mm_set1_pd(3.0);
        prev = select(_mm_cmpeq_pd(c2, best), _mm_set1_pd(2.0), prev);
        prev = select(_mm_cmpeq_pd(c1, best), _mm_set1_pd(1.0), prev);
        prev = select(_mm_cmpeq_pd(c0, best), _mm_setzero_pd(), prev);
        return {.best = best, .prev = select(_mm_cmpgt_pd(best, floor_score), prev, floor_state)};
    }

    // Pack four predecessor indices, held as doubles 0–3 in two halves, into one byte per state.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto pack_states(__m128d low, __m128d high) noexcept -> std::uint32_t {
        const auto words = _mm_unpacklo_epi64(_mm_cvtpd_epi32(low), _mm_cvtpd_epi32(high));
        const auto halves = _mm_packs_epi32(words, words);
        return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(halves, halves)));
    }

    // The sign bit of each lane, lane y in bit y.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto lane_bits(__m128d mask) noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(_mm_movemask_pd(mask));
    }

    // Exchange the two lanes.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto swap_lanes(__m128d pair) noexcept -> __m128d {
        return _mm_shuffle_pd(pair, pair, 1);
    }
};
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
// The vpermpd immediate that moves the earlier (slot 0) or later (slot 1) legal predecessor of state y to lane y.
constexpr
auto legal_permutation(std::size_t slot) noexcept -> int {
    auto order = 0;
    for (auto y = std::size_t{0}; y < legal_predecessors.size(); ++y) {
        order |= static_cast<int>(legal_predecessors[y][slot]) << (2 * y);
    }
    return order;
}

template <>
struct ViterbiKernel<SimdLevel::AVX2> {
    /// Same contract as scalar::viterbi_forward, with one column of four doubles per register. As in the SSE2
    /// score_half, max(later, earlier) keeps the earlier candidate on a tie.
    template <typename State, typename EmitAt>
        requires ViterbiInputs<State, EmitAt>
    static
    auto forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
                 const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
        const auto from_b = _mm256_loadu_pd(transitions[0].data());
        const auto from_e = _mm256_loadu_pd(transitions[1].data());
        const auto from_m = _mm256_loadu_pd(transitions[2].data());
        const auto from_s = _mm256_loadu_pd(transitions[3].data());
        const auto floor_score = _mm256_set1_pd(floor.score);
        const auto floor_state = _mm256_set1_pd(static_cast<double>(std::to_underlying(floor.state)));
        auto current = _mm256_loadu_pd(weights.data());
        for (auto x = std::size_t{1}; x < path.size(); ++x) {
            const auto emit_probs = emit_at(x);
            const auto emit = _mm256_loadu_pd(emit_probs.data());
            const auto c0 = _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, 0x00), from_b), emit);
            const auto c1 = _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, 0x55), from_e), emit);
            const auto c2 = _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, 0xAA), from_m), emit);
            const auto c3 = _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, 0xFF), from_s), emit);
            const auto best = _mm256_max_pd(_mm256_max_pd(c3, c2), _mm256_max_pd(c1, c0));
            auto prev = _mm256_set1_pd(3.0);
            prev = _mm256_blendv_pd(prev, _mm256_set1_pd(2.0), _mm256_cmp_pd(c2, best, _CMP_EQ_OQ));
            prev = _mm256_blendv_pd(prev, _mm256_set1_pd(1.0), _mm256_cmp_pd(c1, best, _CMP_EQ_OQ));
            prev = _mm256_blendv_pd(prev, _mm256_setzero_pd(), _mm256_cmp_pd(c0, best, _CMP_EQ_OQ));
            prev = _mm256_blendv_pd(floor_state, prev, _mm256_cmp_pd(best, floor_score, _CMP_GT_OQ));
            path[x] = std::bit_cast<std::array<State, 4>>(pack_states(prev));
            current = _mm256_max_pd(best, floor_score);
            assert_check(
                [&] {
                    auto scores = StateScores{};
                    _mm256_storeu_pd(scores.data(), current);
                    return finite_scores(scores);
                },
                "HMMSegment: non-finite accumulated weight");
        }
        _mm256_storeu_pd(weights.data(), current);
        return weights;
    }

    /// Same contract as scalar::viterbi_forward_legal, with one column of four doubles per register.
    template <typename State, typename EmitAt>
        requires ViterbiInputs<State, EmitAt>
    static
    auto forward_legal(StateScores weights, std::span<const StateScores, 4> transitions, const EmitAt &emit_at,
                       std::span<std::array<State, 4>> path) -> StateScores {
        constexpr auto earlier_order = legal_permutation(0);
        constexpr auto later_order = legal_permutation(1);
        const auto earlier_rows = legal_transitions(transitions, 0);
        const auto later_rows = legal_transitions(transitions, 1);
        const auto from_earlier = _mm256_loadu_pd(earlier_rows.data());
        const auto from_later = _mm256_loadu_pd(later_rows.data());
        auto current = _mm256_loadu_pd(weights.data());
        for (auto x = std::size_t{1}; x < path.size(); ++x) {
            const auto emit_probs = emit_at(x);
            const auto emit = _mm256_loadu_pd(emit_probs.data());
            const auto earlier =
                _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, earlier_order), from_earlier), emit);
            const auto later =
                _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, later_order), from_later), emit);
            // As in the SSE2 forward_legal, max(earlier, later) keeps the later predecessor on a tie.
            current = _mm256_max_pd(earlier, later);
            path[x] =
                std::bit_cast<std::array<State, 4>>(pack_legal(lane_bits(_mm256_cmp_pd(earlier, later, _CMP_GT_OQ))));
            assert_check(
                [&] {
                    auto scores = StateScores{};
                    _mm256_storeu_pd(scores.data(), current);
                    return finite_scores(scores);
                },
                "HMMSegment: non-finite accumulated weight");
        }
        _mm256_storeu_pd(weights.data(), current);
        return weights;
    }

private:
    // Pack four predecessor indices, held as doubles 0–3, into one byte per state.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto pack_states(__m256d states) noexcept -> std::uint32_t {
        const auto words = _mm256_cvtpd_epi32(states);
        const auto halves = _mm_packs_epi32(words, words);
        return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(halves, halves)));
    }

    // The sign bit of each lane, lane y in bit y.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto lane_bits(__m256d mask) noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(_mm256_movemask_pd(mask));
    }
};
#endif

#if defined(CPPJIEBA_SIMD_NEON)
template <>
struct ViterbiKernel<SimdLevel::NEON> {
    /// Same contract as scalar::viterbi_forward. Each column is two halves, (B, E) and (M, S), of two doubles each.
    template <typename State, typename EmitAt>
        requires ViterbiInputs<State, EmitAt>
    static
    auto forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
                 const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
        const auto rows_low = HalfRows{.from_b = vld1q_f64(transitions[0].data()),
                                       .from_e = vld1q_f64(transitions[1].data()),
                                       .from_m = vld1q_f64(transitions[2].data()),
                                       .from_s = vld1q_f64(transitions[3].data())};
        const auto rows_high = HalfRows{.from_b = vld1q_f64(transitions[0].data() + 2),
                                        .from_e = vld1q_f64(transitions[1].data() + 2),
                                        .from_m = vld1q_f64(transitions[2].data() + 2),
                                        .from_s = vld1q_f64(transitions[3].data() + 2)};
        const auto floor_score = vdupq_n_f64(floor.score);
        const auto floor_state = vdupq_n_u64(static_cast<std::uint64_t>(std::to_underlying(floor.state)));
        auto low = vld1q_f64(weights.data());
        auto high = vld1q_f64(weights.data() + 2);
        for (auto x = std::size_t{1}; x < path.size(); ++x) {
            const auto emit_probs = emit_at(x);
            const auto broadcasts = HalfRows{.from_b = vdupq_laneq_f64(low, 0),
                                             .from_e = vdupq_laneq_f64(low, 1),
                                             .from_m = vdupq_laneq_f64(high, 0),
                                             .from_s = vdupq_laneq_f64(high, 1)};
            const auto first = score_half(broadcasts, rows_low, vld1q_f64(emit_probs.data()), floor_score, floor_state);
            const auto second =
                score_half(broadcasts, rows_high, vld1q_f64(emit_probs.data() + 2), floor_score, floor_state);
            path[x] = std::bit_cast<std::array<State, 4>>(first_word(low_bytes(first.prev, second.prev)));
            low = first.best;
            high = second.best;
            assert_check([&] { return finite_scores(to_scores(low, high)); },
                         "HMMSegment: non-finite accumulated weight");
        }
        return to_scores(low, high);
    }

    /// Same contract as scalar::viterbi_forward_legal, with the lane layout of the SSE2 forward_legal: states (B, E)
    /// weigh predecessors (E, B) against (S, M), and states (M, S) weigh (B, E) against (M, S).
    template <typename State, typename EmitAt>
        requires ViterbiInputs<State, EmitAt>
    static
    auto forward_legal(StateScores weights, std::span<const StateScores, 4> transitions, const EmitAt &emit_at,
                       std::span<std::array<State, 4>> path) -> StateScores {
        const auto earlier_rows = legal_transitions(transitions, 0);
        const auto later_rows = legal_transitions(transitions, 1);
        const auto earlier_low = vld1q_f64(earlier_rows.data());
        const auto earlier_high = vld1q_f64(earlier_rows.data() + 2);
        const auto later_low = vld1q_f64(later_rows.data());
        const auto later_high = vld1q_f64(later_rows.data() + 2);
        const auto earlier_states = vreinterpret_u8_u32(vdup_n_u32(legal_predecessor_bytes(0)));
        const auto later_states = vreinterpret_u8_u32(vdup_n_u32(legal_predecessor_bytes(1)));
        auto low = vld1q_f64(weights.data());
        auto high = vld1q_f64(weights.data() + 2);
        for (auto x = std::size_t{1}; x < path.size(); ++x) {
            const auto emit_probs = emit_at(x);
            const auto emit_low = vld1q_f64(emit_probs.data());
            const auto emit_high = vld1q_f64(emit_probs.data() + 2);
            const auto low_earlier = vaddq_f64(vaddq_f64(swap_lanes(low), earlier_low), emit_low);
            const auto low_later = vaddq_f64(vaddq_f64(swap_lanes(high), later_low), emit_low);
            const auto high_earlier = vaddq_f64(vaddq_f64(low, earlier_high), emit_high);
            const auto high_later = vaddq_f64(vaddq_f64(high, later_high), emit_high);
            // Only a strictly greater earlier score wins, so a tie keeps the later predecessor as the scalar loop does.
            const auto low_wins = vcgtq_f64(low_earlier, low_later);
            const auto high_wins = vcgtq_f64(high_earlier, high_later);
            low = vbslq_f64(low_wins, low_earlier, low_later);
            high = vbslq_f64(high_wins, high_earlier, high_later);
            const auto states = vbsl_u8(low_bytes(low_wins, high_wins), earlier_states, later_states);
            path[x] = std::bit_cast<std::array<State, 4>>(first_word(states));
            assert_check([&] { return finite_scores(to_scores(low, high)); },
                         "HMMSegment: non-finite accumulated weight");
        }
        return to_scores(low, high);
    }

private:
    /// One value per predecessor state for two target states, as in the SSE2 HalfRows.
    struct HalfRows {
        float64x2_t from_b;
        float64x2_t from_e;
        float64x2_t from_m;
        float64x2_t from_s;
    };

    /// The floored best score of two states and the index of its first predecessor, or the floor state.
    struct HalfColumn {
        float64x2_t best;
        uint64x2_t prev;
    };

    // Score two states from the four predecessor weights, each broadcast to both lanes. Each round keeps its later
    // candidate only when strictly greater, so a tie keeps the earlier predecessor as the scalar loop does. vmaxq_f64
    // is avoided: it returns +0.0 when signed zeros tie. The round masks also yield the winner's index.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto score_half(const HalfRows &weights, const HalfRows &rows, float64x2_t emit, float64x2_t floor_score,
                    uint64x2_t floor_state) noexcept -> HalfColumn {
        const auto c0 = vaddq_f64(vaddq_f64(weights.from_b, rows.from_b), emit);
        const auto c1 = vaddq_f64(vaddq_f64(weights.from_e, rows.from_e), emit);
        const auto c2 = vaddq_f64(vaddq_f64(weights.from_m, rows.from_m), emit);
        const auto c3 = vaddq_f64(vaddq_f64(weights.from_s, rows.from_s), emit);
        const auto e_wins = vcgtq_f64(c1, c0);
        const auto s_wins = vcgtq_f64(c3, c2);
        const auto early = vbslq_f64(e_wins, c1, c0);
        const auto late = vbslq_f64(s_wins, c3, c2);
        const auto late_wins = vcgtq_f64(late, early);
        const auto best = vbslq_f64(late_wins, late, early);
        // A set mask lane is all ones: shifting it right by 63 gives E (1) over B (0), and 2 minus it gives S (3) over
        // M.
        const auto early_prev = vshrq_n_u64(e_wins, 63);
        const auto late_prev = vsubq_u64(vdupq_n_u64(2), s_wins);
        const auto above_floor = vcgtq_f64(best, floor_score);
        return {.best = vbslq_f64(above_floor, best, floor_score),
                .prev = vbslq_u64(above_floor, vbslq_u64(late_wins, late_prev, early_prev), floor_state)};
    }

    // The low byte of each 64-bit lane of two halves, in state order, as bytes 0–3.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto low_bytes(uint64x2_t low, uint64x2_t high) noexcept -> uint8x8_t {
        const auto words = vuzp1q_u32(vreinterpretq_u32_u64(low), vreinterpretq_u32_u64(high));
        const auto shorts = vmovn_u32(words);
        return vmovn_u16(vcombine_u16(shorts, shorts));
    }

    // Bytes 0–3 as one little-endian word, the layout of a path row.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto first_word(uint8x8_t bytes) noexcept -> std::uint32_t {
        return vget_lane_u32(vreinterpret_u32_u8(bytes), 0);
    }

    // The four state scores held in two halves.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto to_scores(float64x2_t low, float64x2_t high) noexcept -> StateScores {
        auto scores = StateScores{};
        vst1q_f64(scores.data(), low);
        vst1q_f64(scores.data() + 2, high);
        return scores;
    }

    // Exchange the two lanes.
    [[nodiscard, NEO_FORCE_INLINE]] static
    auto swap_lanes(float64x2_t pair) noexcept -> float64x2_t {
        return vextq_f64(pair, pair, 1);
    }
};
#endif

/// Run the forward pass with Backend, one of compiled_backends; callers pass `level` for the highest. Same contract
/// as scalar::viterbi_forward.
template <SimdLevel Backend, typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline
auto viterbi_forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
                     const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
    static_assert(is_compiled_backend(Backend), "simd::viterbi_forward: this build does not compile the backend");
    if constexpr (Backend == SimdLevel::OFF) {
        return scalar::viterbi_forward(weights, transitions, floor, emit_at, path);
    } else {
        return ViterbiKernel<Backend>::forward(weights, transitions, floor, emit_at, path);
    }
}

/// Run the legal-predecessor forward pass with Backend, one of compiled_backends; callers pass `level` for the
/// highest. Same contract as scalar::viterbi_forward_legal.
template <SimdLevel Backend, typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline
auto viterbi_forward_legal(StateScores weights, std::span<const StateScores, 4> transitions, const EmitAt &emit_at,
                           std::span<std::array<State, 4>> path) -> StateScores {
    static_assert(is_compiled_backend(Backend), "simd::viterbi_forward_legal: this build does not compile the backend");
    if constexpr (Backend == SimdLevel::OFF) {
        return scalar::viterbi_forward_legal(weights, transitions, emit_at, path);
    } else {
        return ViterbiKernel<Backend>::forward_legal(weights, transitions, emit_at, path);
    }
}

} // namespace neo_cppjieba::detail::simd
