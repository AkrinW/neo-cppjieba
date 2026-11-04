#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/Separator.hpp"
#include "neo/resource/HMMState.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/simd/RuneMatch.hpp"
#include "neo/simd/SimdLevel.hpp"
#include "neo/simd/Viterbi.hpp"

#include "SimdExpectation.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;
using namespace neo_cppjieba::detail::simd;

namespace {

// Runes next to the separators in value or in the low bitmask, plus values a signed lane compare would misread and
// the edge of the 16-bit saturation the NEON kernel narrows runes with.
constexpr auto near_separator_runes =
    std::array<char32_t, 18>{8,      11,     31,     33,     0x49,    0x4A,    0x60,    0x2002,     0x3001,
                             0x3003, 0xFF0B, 0xFF0D, 0xFFFF, 0x10000, 0x1FF0C, 0x13002, 0x80000020, 0xFFFFFFFF};

// Matches of the default separators from the backend this build selects.
auto active_matches(std::span<const char32_t> runes) -> std::vector<RuneIndex> {
    auto out = std::vector<RuneIndex>{};
    append_matches(runes, DEFAULT_SEPARATORS, is_default_separator_unlikely, out);
    return out;
}

// Matches of the default separators from the scalar reference.
auto scalar_matches(std::span<const char32_t> runes) -> std::vector<RuneIndex> {
    auto out = std::vector<RuneIndex>{};
    scalar::append_matches(runes, is_default_separator_unlikely, out);
    return out;
}

} // namespace

// The CPPJIEBA_SIMD option, or a variant executable's own definition, reaches the headers as the same level.
TEST(SimdTest, LevelMatchesBuildOption) {
    EXPECT_EQ(level, expected_simd_level);
}

TEST(SimdTest, RuneMatchAppendsAfterExistingIndices) {
    const auto runes = std::u32string{U"中 文，"};
    auto out = std::vector<RuneIndex>{7};
    append_matches(std::span<const char32_t>{runes}, DEFAULT_SEPARATORS, is_default_separator_unlikely, out);
    EXPECT_EQ(out, (std::vector<RuneIndex>{7, 1, 3}));
}

TEST(SimdTest, RuneMatchFindsNothingInEmptyInput) {
    EXPECT_TRUE(active_matches(std::span<const char32_t>{}).empty());
}

// One separator at each position of every length up to three blocks covers each lane, block boundary and tail.
TEST(SimdTest, RuneMatchAgreesWithScalarAtEveryPositionAndLength) {
    for (auto length = std::size_t{0}; length <= 3 * match_block_size + 2; ++length) {
        for (auto position = std::size_t{0}; position < length; ++position) {
            auto runes = std::u32string(length, U'中');
            runes[position] = U'，';
            const auto span = std::span<const char32_t>{runes};
            ASSERT_EQ(active_matches(span), scalar_matches(span)) << "length " << length << ", position " << position;
        }
    }
}

TEST(SimdTest, RuneMatchFindsEveryRuneWhenAllAreSeparators) {
    auto runes = std::u32string{};
    for (auto i = std::size_t{0}; i < 2 * match_block_size + 5; ++i) {
        runes.push_back(DEFAULT_SEPARATORS[i % DEFAULT_SEPARATORS.size()]);
    }
    const auto span = std::span<const char32_t>{runes};
    const auto matches = active_matches(span);
    ASSERT_EQ(matches.size(), runes.size());
    EXPECT_EQ(matches, scalar_matches(span));
}

TEST(SimdTest, RuneMatchIgnoresRunesNearSeparators) {
    auto runes = std::u32string{};
    for (auto i = std::size_t{0}; i < 3 * match_block_size; ++i) {
        runes.push_back(near_separator_runes[i % near_separator_runes.size()]);
    }
    EXPECT_TRUE(active_matches(std::span<const char32_t>{runes}).empty());
}

TEST(SimdTest, RuneMatchAgreesWithScalarOnRandomText) {
    auto engine = std::mt19937{20261103};
    auto length_of = std::uniform_int_distribution<std::size_t>{0, 300};
    auto kind_of = std::uniform_int_distribution<int>{0, 9};
    auto index_of = std::uniform_int_distribution<std::size_t>{0, near_separator_runes.size() - 1};
    for (auto trial = 0; trial < 200; ++trial) {
        auto runes = std::u32string(length_of(engine), U'字');
        for (auto &rune : runes) {
            const auto kind = kind_of(engine);
            if (kind == 0) {
                rune = DEFAULT_SEPARATORS[index_of(engine) % DEFAULT_SEPARATORS.size()];
            } else if (kind == 1) {
                rune = near_separator_runes[index_of(engine)];
            }
        }
        const auto span = std::span<const char32_t>{runes};
        ASSERT_EQ(active_matches(span), scalar_matches(span)) << "trial " << trial;
    }
}

namespace {

// Scores that repeat, cancel to signed zeros, or sit at the floor, so random columns tie often.
constexpr auto viterbi_scores = std::array<double, 8>{-1.5, -2.25, -0.75, 0.0, -0.0, -1.0e-300, 1.5, MIN_DOUBLE};

// The CppJieba-style floor: candidates must exceed MIN_DOUBLE, and E is kept when none does.
constexpr auto cpp_floor = ViterbiFloor<HMMState>{.score = MIN_DOUBLE, .state = HMMState::E};

using PathRow = std::array<HMMState, 4>;

// A forward pass's predecessor rows and final weights, with weights kept as bit patterns so signed zeros differ.
struct ViterbiResult {
    std::vector<PathRow> path;
    std::array<std::uint64_t, 4> weight_bits;

    auto operator==(const ViterbiResult &) const -> bool = default;
};

// Run one backend over emissions[1..]; every row starts as S so a row the pass skips stays visible.
template <typename Forward>
auto run_viterbi(const Forward &forward, const StateScores &start, const std::array<StateScores, 4> &transitions,
                 const std::vector<StateScores> &emissions) -> ViterbiResult {
    auto path = std::vector<PathRow>(emissions.size(), PathRow{HMMState::S, HMMState::S, HMMState::S, HMMState::S});
    const auto emit_at = [&](std::size_t x) {
        return emissions[x];
    };
    const auto weights = forward(start, std::span{transitions}, emit_at, std::span{path});
    auto bits = std::array<std::uint64_t, 4>{};
    for (auto y = std::size_t{0}; y < bits.size(); ++y) {
        bits[y] = std::bit_cast<std::uint64_t>(weights[y]);
    }
    return {.path = std::move(path), .weight_bits = bits};
}

// The CppJieba-rule backend this build selects with a given floor, and the scalar reference every backend must match.
constexpr auto active_forward = [](ViterbiFloor<HMMState> floor) {
    return [floor](const auto &start, auto transitions, const auto &emit_at, auto path) {
        return viterbi_forward(start, transitions, floor, emit_at, path);
    };
};
constexpr auto scalar_forward = [](ViterbiFloor<HMMState> floor) {
    return [floor](const auto &start, auto transitions, const auto &emit_at, auto path) {
        return scalar::viterbi_forward(start, transitions, floor, emit_at, path);
    };
};

// The legal-predecessor backend this build selects, and its scalar reference.
constexpr auto active_legal_forward = [](const auto &start, auto transitions, const auto &emit_at, auto path) {
    return viterbi_forward_legal(start, transitions, emit_at, path);
};
constexpr auto scalar_legal_forward = [](const auto &start, auto transitions, const auto &emit_at, auto path) {
    return scalar::viterbi_forward_legal(start, transitions, emit_at, path);
};

// Fill four scores from the shared pool.
auto random_scores(std::mt19937 &engine) -> StateScores {
    auto index_of = std::uniform_int_distribution<std::size_t>{0, viterbi_scores.size() - 1};
    return {viterbi_scores[index_of(engine)], viterbi_scores[index_of(engine)], viterbi_scores[index_of(engine)],
            viterbi_scores[index_of(engine)]};
}

// The column-0 weights, transition rows and per-column emissions of one forward pass.
struct ViterbiInput {
    StateScores start;
    std::array<StateScores, 4> transitions;
    std::vector<StateScores> emissions;
};

// Draw a run of 1 to 24 columns from the score pool; about one column in five is a rune absent from the emission
// table.
auto random_input(std::mt19937 &engine) -> ViterbiInput {
    auto length_of = std::uniform_int_distribution<std::size_t>{1, 24};
    auto absent_of = std::uniform_int_distribution<int>{0, 4};
    auto input = ViterbiInput{.start = random_scores(engine),
                              .transitions = std::array{random_scores(engine), random_scores(engine),
                                                        random_scores(engine), random_scores(engine)},
                              .emissions = std::vector<StateScores>(length_of(engine))};
    for (auto &column : input.emissions) {
        column = absent_of(engine) == 0 ? StateScores{MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE}
                                        : random_scores(engine);
    }
    return input;
}

} // namespace

TEST(SimdTest, ViterbiAgreesWithScalarOnRandomColumns) {
    auto engine = std::mt19937{20261103};
    for (auto trial = 0; trial < 2000; ++trial) {
        const auto input = random_input(engine);
        ASSERT_EQ(run_viterbi(active_forward(cpp_floor), input.start, input.transitions, input.emissions),
                  run_viterbi(scalar_forward(cpp_floor), input.start, input.transitions, input.emissions))
            << "trial " << trial;
    }
}

TEST(SimdTest, ViterbiAgreesWithScalarForAnyFloor) {
    auto engine = std::mt19937{20261104};
    auto score_of = std::uniform_int_distribution<std::size_t>{0, viterbi_scores.size() - 1};
    auto state_of = std::uniform_int_distribution<int>{0, 3};
    for (auto trial = 0; trial < 2000; ++trial) {
        const auto floor = ViterbiFloor<HMMState>{.score = viterbi_scores[score_of(engine)],
                                                  .state = static_cast<HMMState>(state_of(engine))};
        const auto input = random_input(engine);
        ASSERT_EQ(run_viterbi(active_forward(floor), input.start, input.transitions, input.emissions),
                  run_viterbi(scalar_forward(floor), input.start, input.transitions, input.emissions))
            << "trial " << trial;
    }
}

TEST(SimdTest, ViterbiKeepsFloorStateWhenNoCandidateExceedsTheFloor) {
    const auto row = StateScores{-1.0, -2.0, -3.0, -4.0};
    const auto absent = StateScores{MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE};
    const auto result =
        run_viterbi(active_forward(cpp_floor), row, std::array{row, row, row, row}, {absent, absent, absent});
    const auto floor_row = PathRow{HMMState::E, HMMState::E, HMMState::E, HMMState::E};
    EXPECT_EQ(result.path[1], floor_row);
    EXPECT_EQ(result.path[2], floor_row);
    const auto floor_bits = std::bit_cast<std::uint64_t>(MIN_DOUBLE);
    EXPECT_EQ(result.weight_bits, (std::array{floor_bits, floor_bits, floor_bits, floor_bits}));
}

TEST(SimdTest, ViterbiTakesTheFirstPredecessorOnTies) {
    const auto row = StateScores{-1.0, -1.0, -1.0, -1.0};
    const auto result = run_viterbi(active_forward(cpp_floor), row, std::array{row, row, row, row}, {row, row});
    EXPECT_EQ(result.path[1], (PathRow{HMMState::B, HMMState::B, HMMState::B, HMMState::B}));
}

// From B every candidate is -0.0 and from E +0.0; they compare equal, so B, the earlier one, keeps its -0.0 score.
TEST(SimdTest, ViterbiKeepsTheEarlierScoreWhenSignedZerosTie) {
    const auto start = StateScores{-0.0, 0.0, -1.0, -1.0};
    const auto from_b = StateScores{-0.0, -0.0, -0.0, -0.0};
    const auto from_e = StateScores{0.0, 0.0, 0.0, 0.0};
    const auto from_other = StateScores{-1.0, -1.0, -1.0, -1.0};
    const auto emit = StateScores{-0.0, -0.0, -0.0, -0.0};
    const auto result =
        run_viterbi(active_forward(cpp_floor), start, std::array{from_b, from_e, from_other, from_other}, {emit, emit});
    const auto negative_zero = std::bit_cast<std::uint64_t>(-0.0);
    EXPECT_EQ(result.path[1], (PathRow{HMMState::B, HMMState::B, HMMState::B, HMMState::B}));
    EXPECT_EQ(result.weight_bits, (std::array{negative_zero, negative_zero, negative_zero, negative_zero}));
}

TEST(SimdTest, ViterbiOverOneColumnReturnsTheStartWeightsAndWritesNoRow) {
    const auto start = StateScores{-1.0, -0.0, 0.0, -2.0};
    const auto row = StateScores{-1.0, -1.0, -1.0, -1.0};
    const auto result = run_viterbi(active_forward(cpp_floor), start, std::array{row, row, row, row}, {row});
    EXPECT_EQ(result.path[0], (PathRow{HMMState::S, HMMState::S, HMMState::S, HMMState::S}));
    EXPECT_EQ(result.weight_bits, (std::array{std::bit_cast<std::uint64_t>(-1.0), std::bit_cast<std::uint64_t>(-0.0),
                                              std::bit_cast<std::uint64_t>(0.0), std::bit_cast<std::uint64_t>(-2.0)}));
}

TEST(SimdTest, ViterbiLegalAgreesWithScalarOnRandomColumns) {
    auto engine = std::mt19937{20261105};
    for (auto trial = 0; trial < 2000; ++trial) {
        const auto input = random_input(engine);
        ASSERT_EQ(run_viterbi(active_legal_forward, input.start, input.transitions, input.emissions),
                  run_viterbi(scalar_legal_forward, input.start, input.transitions, input.emissions))
            << "trial " << trial;
    }
}

TEST(SimdTest, ViterbiLegalTakesTheLaterPredecessorOnTies) {
    const auto row = StateScores{-1.0, -1.0, -1.0, -1.0};
    const auto result = run_viterbi(active_legal_forward, row, std::array{row, row, row, row}, {row, row});
    EXPECT_EQ(result.path[1], (PathRow{HMMState::S, HMMState::M, HMMState::M, HMMState::S}));
}

// Earlier predecessors (E for B and S, B for E and M) score +0.0 and later ones -0.0; the later -0.0 is kept.
TEST(SimdTest, ViterbiLegalKeepsTheLaterScoreWhenSignedZerosTie) {
    const auto start = StateScores{0.0, 0.0, -0.0, -0.0};
    const auto row = StateScores{-0.0, -0.0, -0.0, -0.0};
    const auto result = run_viterbi(active_legal_forward, start, std::array{row, row, row, row}, {row, row});
    const auto negative_zero = std::bit_cast<std::uint64_t>(-0.0);
    EXPECT_EQ(result.path[1], (PathRow{HMMState::S, HMMState::M, HMMState::M, HMMState::S}));
    EXPECT_EQ(result.weight_bits, (std::array{negative_zero, negative_zero, negative_zero, negative_zero}));
}
