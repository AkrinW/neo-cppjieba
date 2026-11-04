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

#if defined(CPPJIEBA_SIMD_AVX2) || defined(CPPJIEBA_SIMD_SSE2)
#include "neo/util/ForceInline.hpp"

#include <bit>
#include <cstdint>
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
#include <immintrin.h>
#elif defined(CPPJIEBA_SIMD_SSE2)
#include <emmintrin.h>
#endif

// Forward-pass kernels for the Viterbi in algorithm/HMMSegment.hpp. Each column scores the states B, E, M, S. Under
// the CppJieba rule a state keeps the first predecessor whose (weight + transition) + emission strictly exceeds the
// floor and every earlier candidate; under the Python/Rust rule it weighs only its two legal predecessors and keeps
// the later one on a tie. Vector backends add in the same order and break ties the same way, so they match the scalar
// loops bit for bit; NEON builds use the scalar loops for now.

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
inline auto finite_scores(const StateScores &scores) noexcept -> bool {
    return std::ranges::all_of(scores, [](double score) { return std::isfinite(score); });
}

namespace scalar {

/// Score columns 1 .. path.size() - 1 from the column-0 weights; path[x][y] receives the predecessor of state y at
/// column x, and the result holds the last column's weights. Every backend must reproduce this result exactly.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
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
inline auto viterbi_forward_legal(StateScores weights, std::span<const StateScores, 4> transitions,
                                  const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
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

#if defined(CPPJIEBA_SIMD_AVX2) || defined(CPPJIEBA_SIMD_SSE2)

// Move bit y of a four-bit lane mask to the low bit of byte y; the four shifted copies of the mask never overlap.
[[nodiscard, NEO_FORCE_INLINE]]
constexpr auto spread_lanes(std::uint32_t mask) noexcept -> std::uint32_t {
    return (mask * std::uint32_t{0x00204081}) & std::uint32_t{0x01010101};
}

// The earlier (slot 0) or later (slot 1) legal predecessor of every state, one byte per state.
constexpr auto legal_predecessor_bytes(std::size_t slot) noexcept -> std::uint32_t {
    auto bytes = std::uint32_t{0};
    for (auto y = std::size_t{0}; y < legal_predecessors.size(); ++y) {
        bytes |= static_cast<std::uint32_t>(legal_predecessors[y][slot]) << (8 * y);
    }
    return bytes;
}

// Byte y receives the earlier legal predecessor of state y where earlier_wins has bit y, else the later one.
[[nodiscard, NEO_FORCE_INLINE]]
constexpr auto pack_legal(std::uint32_t earlier_wins) noexcept -> std::uint32_t {
    constexpr auto later_bytes = legal_predecessor_bytes(1);
    // Every legal pair is two states apart, so one subtraction per byte picks the earlier state.
    static_assert(later_bytes - legal_predecessor_bytes(0) == 0x02020202U);
    return later_bytes - 2 * spread_lanes(earlier_wins);
}

// Lane y holds the transition into state y from its earlier (slot 0) or later (slot 1) legal predecessor.
inline auto legal_transitions(std::span<const StateScores, 4> transitions, std::size_t slot) noexcept -> StateScores {
    auto rows = StateScores{};
    for (auto y = std::size_t{0}; y < rows.size(); ++y) {
        rows[y] = transitions[legal_predecessors[y][slot]][y];
    }
    return rows;
}

// AVX2 builds compile this backend too, so tests and measurements can compare both widths in one build.
namespace sse2 {

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
[[nodiscard, NEO_FORCE_INLINE]]
inline auto select(__m128d mask, __m128d a, __m128d b) noexcept -> __m128d {
    return _mm_or_pd(_mm_and_pd(mask, a), _mm_andnot_pd(mask, b));
}

// Score two states from the four predecessor weights, each broadcast to both lanes. _mm_max_pd(a, b) returns b
// unless a > b, so max(later, earlier) keeps the earlier candidate on a tie, as the scalar loop does.
[[nodiscard, NEO_FORCE_INLINE]]
inline auto score_half(const HalfRows &weights, const HalfRows &rows, __m128d emit, __m128d floor_score,
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
[[nodiscard, NEO_FORCE_INLINE]]
inline auto pack_states(__m128d low, __m128d high) noexcept -> std::uint32_t {
    const auto words = _mm_unpacklo_epi64(_mm_cvtpd_epi32(low), _mm_cvtpd_epi32(high));
    const auto halves = _mm_packs_epi32(words, words);
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(halves, halves)));
}

/// Same contract as scalar::viterbi_forward. Each column is two halves, (B, E) and (M, S), of two doubles each.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
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
        const auto first = score_half(broadcasts, rows_low, _mm_loadu_pd(emit_probs.data()), floor_score, floor_state);
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

// The sign bit of each lane, lane y in bit y.
[[nodiscard, NEO_FORCE_INLINE]]
inline auto lane_bits(__m128d mask) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(_mm_movemask_pd(mask));
}

// Exchange the two lanes.
[[nodiscard, NEO_FORCE_INLINE]]
inline auto swap_lanes(__m128d pair) noexcept -> __m128d {
    return _mm_shuffle_pd(pair, pair, 1);
}

/// Same contract as scalar::viterbi_forward_legal. States (B, E) weigh predecessors (E, B) against (S, M), and
/// states (M, S) weigh (B, E) against (M, S).
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward_legal(StateScores weights, std::span<const StateScores, 4> transitions,
                                  const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
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
        const auto earlier_wins =
            lane_bits(_mm_cmpgt_pd(low_earlier, low_later)) | (lane_bits(_mm_cmpgt_pd(high_earlier, high_later)) << 2);
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

} // namespace sse2
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
namespace avx2 {

// Pack four predecessor indices, held as doubles 0–3, into one byte per state.
[[nodiscard, NEO_FORCE_INLINE]]
inline auto pack_states(__m256d states) noexcept -> std::uint32_t {
    const auto words = _mm256_cvtpd_epi32(states);
    const auto halves = _mm_packs_epi32(words, words);
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(halves, halves)));
}

/// Same contract as scalar::viterbi_forward, with one column of four doubles per register. As in sse2::score_half,
/// max(later, earlier) keeps the earlier candidate on a tie.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
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

// The sign bit of each lane, lane y in bit y.
[[nodiscard, NEO_FORCE_INLINE]]
inline auto lane_bits(__m256d mask) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(_mm256_movemask_pd(mask));
}

// The vpermpd immediate that moves the earlier (slot 0) or later (slot 1) legal predecessor of state y to lane y.
constexpr auto legal_permutation(std::size_t slot) noexcept -> int {
    auto order = 0;
    for (auto y = std::size_t{0}; y < legal_predecessors.size(); ++y) {
        order |= static_cast<int>(legal_predecessors[y][slot]) << (2 * y);
    }
    return order;
}

/// Same contract as scalar::viterbi_forward_legal, with one column of four doubles per register.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward_legal(StateScores weights, std::span<const StateScores, 4> transitions,
                                  const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
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
        const auto later = _mm256_add_pd(_mm256_add_pd(_mm256_permute4x64_pd(current, later_order), from_later), emit);
        // As in sse2::viterbi_forward_legal, max(earlier, later) keeps the later predecessor on a tie.
        current = _mm256_max_pd(earlier, later);
        path[x] = std::bit_cast<std::array<State, 4>>(pack_legal(lane_bits(_mm256_cmp_pd(earlier, later, _CMP_GT_OQ))));
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

} // namespace avx2
#endif

/// Run the forward pass with the highest backend the build enables; same contract as scalar::viterbi_forward.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward(StateScores weights, std::span<const StateScores, 4> transitions, ViterbiFloor<State> floor,
                            const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
#if defined(CPPJIEBA_SIMD_AVX2)
    return avx2::viterbi_forward(weights, transitions, floor, emit_at, path);
#elif defined(CPPJIEBA_SIMD_SSE2)
    return sse2::viterbi_forward(weights, transitions, floor, emit_at, path);
#else
    static_assert(level == SimdLevel::OFF || level == SimdLevel::NEON,
                  "simd::viterbi_forward: a level with a vector kernel fell through to the scalar loop");
    return scalar::viterbi_forward(weights, transitions, floor, emit_at, path);
#endif
}

/// Run the legal-predecessor forward pass with the highest backend the build enables; same contract as
/// scalar::viterbi_forward_legal.
template <typename State, typename EmitAt>
    requires ViterbiInputs<State, EmitAt>
inline auto viterbi_forward_legal(StateScores weights, std::span<const StateScores, 4> transitions,
                                  const EmitAt &emit_at, std::span<std::array<State, 4>> path) -> StateScores {
#if defined(CPPJIEBA_SIMD_AVX2)
    return avx2::viterbi_forward_legal(weights, transitions, emit_at, path);
#elif defined(CPPJIEBA_SIMD_SSE2)
    return sse2::viterbi_forward_legal(weights, transitions, emit_at, path);
#else
    static_assert(level == SimdLevel::OFF || level == SimdLevel::NEON,
                  "simd::viterbi_forward_legal: a level with a vector kernel fell through to the scalar loop");
    return scalar::viterbi_forward_legal(weights, transitions, emit_at, path);
#endif
}

} // namespace neo_cppjieba::detail::simd
