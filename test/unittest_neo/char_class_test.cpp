#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/CharClass.hpp"
#include "neo/value/Rune.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;
using namespace std::string_view_literals;

namespace {

// An inclusive rune range.
struct RuneRange {
    Rune first;
    Rune last;
};

// The blocks jieba-rs 0.11.0 treats as CJK in dictionary blocks.
constexpr auto rust_cjk_blocks =
    std::array{RuneRange{0x3400, 0x4DBF},   RuneRange{0x4E00, 0x9FFF},   RuneRange{0xF900, 0xFAFF},
               RuneRange{0x20000, 0x2A6DF}, RuneRange{0x2A700, 0x2B73F}, RuneRange{0x2B740, 0x2B81F},
               RuneRange{0x2B820, 0x2CEAF}, RuneRange{0x2CEB0, 0x2EBEF}, RuneRange{0x2F800, 0x2FA1F}};

// Every rune Python 3.14's str.isspace() accepts (Unicode 16.0), generated with that interpreter.
constexpr auto python_spaces = std::array{
    RuneRange{0x09, 0x0D},     RuneRange{0x1C, 0x20},     RuneRange{0x85, 0x85},     RuneRange{0xA0, 0xA0},
    RuneRange{0x1680, 0x1680}, RuneRange{0x2000, 0x200A}, RuneRange{0x2028, 0x2029}, RuneRange{0x202F, 0x202F},
    RuneRange{0x205F, 0x205F}, RuneRange{0x3000, 0x3000}};

// Whether a rune lies in any of the ranges.
auto in_ranges(std::span<const RuneRange> ranges, Rune rune) -> bool {
    return std::ranges::any_of(ranges,
                               [rune](const RuneRange &range) { return range.first <= rune && rune <= range.last; });
}

} // namespace

TEST(CharClassTest, RustCjkMatchesTheJiebaRsBlocks) {
    for (auto rune = Rune{0}; rune <= Rune{0x110000}; ++rune) {
        ASSERT_EQ(is_rust_cjk(rune), in_ranges(rust_cjk_blocks, rune)) << static_cast<std::uint32_t>(rune);
    }
}

TEST(CharClassTest, PythonSpaceMatchesPythonIsspace) {
    for (auto rune = Rune{0}; rune <= Rune{0x110000}; ++rune) {
        ASSERT_EQ(is_python_space(rune), in_ranges(python_spaces, rune)) << static_cast<std::uint32_t>(rune);
    }
}

// Spot checks at block edges against Python 3.14's str.isdecimal(), including runes below every block.
TEST(CharClassTest, PythonDecimalAcceptsWholeDigitBlocksOnly) {
    for (const auto rune : {U'0', U'9', Rune{0x660}, Rune{0x669}, Rune{0x1D7CE}, Rune{0x1D7FF}, Rune{0x1E5F1},
                            Rune{0x1E5FA}, Rune{0x1FBF0}, Rune{0x1FBF9}}) {
        EXPECT_TRUE(is_python_decimal(rune)) << static_cast<std::uint32_t>(rune);
    }
    for (const auto rune : {Rune{0}, U'/', U':', Rune{0x65F}, Rune{0x66A}, Rune{0x1D7CD}, Rune{0x1D800}, Rune{0x1E5F0},
                            Rune{0x1E5FB}, Rune{0x1FBFA}}) {
        EXPECT_FALSE(is_python_decimal(rune)) << static_cast<std::uint32_t>(rune);
    }
}

// Outside Rust style, an ASCII HMM token continues through a decimal fraction and takes one trailing percent sign.
TEST(CharClassTest, HmmAsciiTokenTakesDecimalFractionAndPercent) {
    if constexpr (compile_config::segmentation_style == SegmentationStyle::RUST) {
        GTEST_SKIP() << "Rust style joins connectors instead";
    }
    const auto token_end = [](std::u32string_view text) {
        return style_hmm_ascii_end(std::span{text}, 0);
    };
    EXPECT_EQ(token_end(U"v3.14%x"sv), 6U);
    EXPECT_EQ(token_end(U"v3.x"sv), 2U);
    EXPECT_EQ(token_end(U"v3."sv), 2U);
    EXPECT_EQ(token_end(U"ab%%"sv), 3U);
}
