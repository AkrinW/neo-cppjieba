#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/Separator.hpp"
#include "neo/simd/RuneMatch.hpp"
#include "neo/simd/SimdLevel.hpp"

#include "SimdExpectation.hpp"

#include <array>
#include <cstddef>
#include <random>
#include <span>
#include <string>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;
using namespace neo_cppjieba::detail::simd;

namespace {

// Runes next to the separators in value or in the low bitmask, plus values a signed lane compare would misread.
constexpr auto near_separator_runes = std::array<char32_t, 16>{
    8, 11, 31, 33, 0x49, 0x4A, 0x60, 0x2002, 0x3001, 0x3003, 0xFF0B, 0xFF0D, 0x1FF0C, 0x13002, 0x80000020, 0xFFFFFFFF};

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
