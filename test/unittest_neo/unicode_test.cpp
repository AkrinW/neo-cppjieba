#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Utf.hpp"
#include "neo/util/Unicode.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

namespace {

// Explicit UTF-8 octets keep byte tests independent of char signedness and the execution character set.
template <ByteType ByteT>
constexpr auto sample_utf8_bytes() -> std::array<ByteT, 12> {
    constexpr auto octets = std::array{0x41, 0x00, 0xC2, 0xA2, 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80, 0x00};
    auto result = std::array<ByteT, octets.size()>{};
    for (auto i = std::size_t{0}; i < octets.size(); ++i) {
        result[i] = static_cast<ByteT>(octets[i]);
    }
    return result;
}

// Every byte representation must share UTF-8 validation and offset semantics.
template <typename ByteT>
class UnicodeByteTest : public ::testing::Test {};

using ByteTypes = ::testing::Types<std::byte, unsigned char, signed char>;
TYPED_TEST_SUITE(UnicodeByteTest, ByteTypes);

} // namespace

TYPED_TEST(UnicodeByteTest, DecodesKnownUtf8OctetsWithByteOffsets) {
    const auto input = sample_utf8_bytes<TypeParam>();
    const auto expected = Unicode{U'A', U'\0', U'¢', U'中', U'😀', U'\0'};
    const auto decoded = decode_with_offset(input);
    EXPECT_TRUE(is_valid_utf(input));
    EXPECT_EQ(decode_one(input), U'A');
    EXPECT_EQ(decode(input), expected);
    EXPECT_EQ(decoded.runes, expected);
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0, 1, 2, 4, 7, 11, 12}));
}

TYPED_TEST(UnicodeByteTest, FixedByteArraysRetainLeadingAndTrailingNul) {
    const TypeParam input[] = {TypeParam{}, static_cast<TypeParam>(0x41), TypeParam{}};
    EXPECT_EQ(decode_one(input), U'\0');
    EXPECT_EQ(decode(input), (Unicode{U'\0', U'A', U'\0'}));
}

TYPED_TEST(UnicodeByteTest, SpanDecodingRespectsItsExplicitBounds) {
    const auto input = sample_utf8_bytes<TypeParam>();
    const auto complete = std::span{input.data() + 4, std::size_t{3}};
    const auto truncated = complete.first(2);
    EXPECT_TRUE(is_valid_utf(complete));
    EXPECT_FALSE(is_valid_utf(truncated));
    EXPECT_EQ(decode(complete), (Unicode{U'中'}));
    EXPECT_THROW(decode_one(truncated), LogConfig::Exception);
    EXPECT_THROW(decode(truncated), LogConfig::Exception);
    EXPECT_THROW(decode_with_offset(truncated), LogConfig::Exception);
}

TYPED_TEST(UnicodeByteTest, DecodesOwningTemporaryBuffers) {
    const auto input = sample_utf8_bytes<TypeParam>();
    EXPECT_EQ(decode(std::vector<TypeParam>(input.begin(), input.end())),
              (Unicode{U'A', U'\0', U'¢', U'中', U'😀', U'\0'}));
}

TYPED_TEST(UnicodeByteTest, EmptyBuffersHaveOnlyTheSentinelOffset) {
    const auto input = std::span<const TypeParam>{};
    const auto decoded = decode_with_offset(input);
    EXPECT_TRUE(decode(input).empty());
    EXPECT_TRUE(decoded.runes.empty());
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0}));
    EXPECT_THROW(decode_one(input), LogConfig::Exception);
}

TYPED_TEST(UnicodeByteTest, InvalidUtf8ThrowsTheConfiguredException) {
    const auto invalid = std::array{std::array{0xC0, 0xAF, 0x00, 0x00}, std::array{0xED, 0xA0, 0x80, 0x00},
                                    std::array{0xF4, 0x90, 0x80, 0x80}, std::array{0xE4, 0x28, 0xAD, 0x00},
                                    std::array{0xFF, 0x00, 0x00, 0x00}};
    for (const auto &octets : invalid) {
        auto input = std::array<TypeParam, 4>{};
        for (auto i = std::size_t{0}; i < octets.size(); ++i) {
            input[i] = static_cast<TypeParam>(octets[i]);
        }
        EXPECT_FALSE(is_valid_utf(input));
        EXPECT_THROW(decode_one(input), LogConfig::Exception);
        EXPECT_THROW(decode(input), LogConfig::Exception);
        EXPECT_THROW(decode_with_offset(input), LogConfig::Exception);
    }
}

TYPED_TEST(UnicodeByteTest, DecodeOneSupportsConstantEvaluation) {
    constexpr auto valid = [] {
        const auto input = sample_utf8_bytes<TypeParam>();
        const auto units = as_code_units(input);
        return decode_one(units.subspan(2, 2)) == U'¢' && decode_one(units.subspan(4, 3)) == U'中'
               && decode_one(units.subspan(7, 4)) == U'😀';
    }();
    static_assert(valid);
    EXPECT_TRUE(valid);
}

TEST(UnicodeTest, StandardByteViewsDecodeUtf8Storage) {
    const auto input = std::u8string_view{u8"中😀"};
    const auto bytes = std::as_bytes(std::span{input.data(), input.size()});
    EXPECT_EQ(decode(bytes), (Unicode{U'中', U'😀'}));

    char8_t writable[] = {u8'A', u8'\0'};
    EXPECT_EQ(decode(std::as_writable_bytes(std::span{writable})), (Unicode{U'A', U'\0'}));
}

TEST(UnicodeWithSourceTest, SafetyChecksRejectTruncatedUtf8) {
    const auto input = std::string{"a\xE4\xBD"};
    EXPECT_THROW(decode(input), LogConfig::Exception);
    EXPECT_THROW(decode_with_offset(input), LogConfig::Exception);
}

TEST(UnicodeWithSourceTest, SafetyChecksRejectTruncatedUtf16) {
    const auto input = std::u16string{u'a', char16_t{0xD800}};
    EXPECT_THROW(decode(input), LogConfig::Exception);
    EXPECT_THROW(decode_with_offset(input), LogConfig::Exception);
}

namespace {

// Compiler-produced UTF literals provide independent expectations for every character type.
template <CharType CharT>
constexpr auto sample_text() -> std::basic_string_view<CharT> {
    if constexpr (std::same_as<CharT, char>) {
        return as_view("A\0中😀");
    } else if constexpr (std::same_as<CharT, char8_t>) {
        return as_view(u8"A\0中😀");
    } else if constexpr (std::same_as<CharT, char16_t>) {
        return as_view(u"A\0中😀");
    } else if constexpr (std::same_as<CharT, char32_t>) {
        return as_view(U"A\0中😀");
    } else {
        return as_view(L"A\0中😀");
    }
}

// Borrowing decoded storage requires its owner to survive the full expression.
template <typename T>
concept CanBorrowRunes = requires(T &&decoded) { std::forward<T>(decoded).get_runes(); };

// Offset references have the same owner lifetime requirement as rune references.
template <typename T>
concept CanBorrowOffsets = requires(T &&decoded) { std::forward<T>(decoded).get_offsets(); };

// All declared character types must support the same public Unicode operations.
template <typename CharT>
class UnicodeCharacterTest : public ::testing::Test {};

} // namespace

using UnicodeCharacterTypes = ::testing::Types<char, char8_t, char16_t, char32_t, wchar_t>;
TYPED_TEST_SUITE(UnicodeCharacterTest, UnicodeCharacterTypes);

TYPED_TEST(UnicodeCharacterTest, TextInterfacesRejectRawCharacterPointers) {
    using Input = const TypeParam *;
    static_assert(!requires(Input input) { is_valid_utf(input); });
    static_assert(!requires(Input input) { decode_one(input); });
    static_assert(!requires(Input input) { decode(input); });
    static_assert(!requires(Input input) { decode_with_offset(input); });
    static_assert(!requires(Input input, UnicodeWithOffset &out) { decode_with_offset_into(input, out); });
}

TEST(UnicodeTest, LiteralDecodingPreservesEmbeddedNul) {
    EXPECT_EQ(decode("A\0中"), (Unicode{U'A', U'\0', U'中'}));
}

TEST(UnicodeTest, ExplicitlySizedPointerDecodingPreservesEmbeddedNul) {
    const char *input = "A\0中";
    const auto expected = Unicode{U'A', U'\0', U'中'};
    EXPECT_EQ(decode(std::string_view{input, 5}), expected);
    EXPECT_EQ(decode(std::span{input, std::size_t{5}}), expected);
}

TYPED_TEST(UnicodeCharacterTest, DecodeIntoReplacesContentsAndRetainsCapacity) {
    auto buffer = decode_with_offset("old text");
    buffer.runes.reserve(64);
    buffer.offsets.reserve(64);
    const auto rune_capacity = buffer.runes.capacity();
    const auto offset_capacity = buffer.offsets.capacity();
    const auto input = sample_text<TypeParam>();
    const auto expected = decode_with_offset(input);
    decode_with_offset_into(input, buffer);
    EXPECT_EQ(buffer.runes, expected.runes);
    EXPECT_EQ(buffer.offsets, expected.offsets);
    EXPECT_EQ(buffer.runes.capacity(), rune_capacity);
    EXPECT_EQ(buffer.offsets.capacity(), offset_capacity);
}

TEST(UnicodeWithSourceTest, EmptyDecodeIntoReplacesPreviousContentWithSentinel) {
    auto buffer = decode_with_offset("中国");
    decode_with_offset_into(std::string_view{}, buffer);
    EXPECT_TRUE(buffer.runes.empty());
    EXPECT_EQ(buffer.offsets, (std::vector<SourceOffset>{0}));
}

TEST(UnicodeWithSourceTest, DecodeIntoCanReuseBuffersAfterInvalidInput) {
    auto buffer = UnicodeWithOffset{};
    EXPECT_THROW(decode_with_offset_into(std::string_view{"中\xE4\xB8"}, buffer), LogConfig::Exception);
    decode_with_offset_into("😀", buffer);
    EXPECT_EQ(buffer.runes, (Unicode{U'😀'}));
    EXPECT_EQ(buffer.offsets, (std::vector<SourceOffset>{0, 4}));
}

TYPED_TEST(UnicodeCharacterTest, MatchesKnownEncodedUnitsAndSourceOffsets) {
    const auto input = sample_text<TypeParam>();
    const auto expected = Unicode{U'A', U'\0', U'中', U'😀'};
    const auto decoded = decode_with_offset(input);
    EXPECT_EQ(decoded.runes, expected);
    EXPECT_EQ(decode(input), expected);
    EXPECT_EQ(encode<TypeParam>(std::span<const Rune>{expected}), (std::basic_string<TypeParam>{input}));
    ASSERT_EQ(decoded.offsets.size(), expected.size() + 1);
    EXPECT_EQ(decoded.offsets.front(), 0u);
    EXPECT_EQ(decoded.offsets.back(), input.size());

    for (auto i = size_t{0}; i < expected.size(); ++i) {
        const auto range = WordRange{static_cast<RuneIndex>(i), static_cast<RuneIndex>(i + 1)};
        const auto one = encode_one<TypeParam>(expected[i]);
        EXPECT_EQ(encode(input, decoded.offsets, range), one);
        EXPECT_EQ(decoded.offsets[i + 1] - decoded.offsets[i], one.size());
        EXPECT_EQ(decode_one(one), expected[i]);
    }
}

TYPED_TEST(UnicodeCharacterTest, ScalarBoundariesRoundTrip) {
    const auto runes = Unicode{0, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFE, 0xFFFF, 0x10000, 0x10FFFF};
    const auto encoded = encode<TypeParam>(std::span<const Rune>{runes});
    EXPECT_EQ(decode(encoded), runes);
}

TYPED_TEST(UnicodeCharacterTest, WordRangeSliceEncodesSelectedRunes) {
    const auto runes = std::array{U'A', U'\0', U'中', U'😀'};
    const auto range = WordRange{1, 4};
    EXPECT_EQ(Utf::encode<TypeParam>(range.slice(runes)),
              (std::basic_string<TypeParam>{sample_text<TypeParam>().substr(1)}));
}

TYPED_TEST(UnicodeCharacterTest, EmptyWordRangeSliceEncodesToEmptyString) {
    const auto runes = std::array{U'中', U'😀'};
    const auto range = WordRange{1, 1};
    EXPECT_TRUE(Utf::encode<TypeParam>(range.slice(runes)).empty());
}

TYPED_TEST(UnicodeCharacterTest, InvalidScalarsThrowConfiguredException) {
    for (const auto rune : std::array<Rune, 3>{0xD800, 0xDFFF, 0x110000}) {
        const auto runes = std::array{U'A', rune};
        EXPECT_THROW(encode_one<TypeParam>(rune), LogConfig::Exception);
        EXPECT_THROW(encode<TypeParam>(std::span<const Rune>{runes}), LogConfig::Exception);
    }
}

TYPED_TEST(UnicodeCharacterTest, EmptySingleRuneInputThrowsAtThePublicEntry) {
    const auto input = std::basic_string_view<TypeParam>{};
    EXPECT_THROW(decode_one(input), LogConfig::Exception);
}

TYPED_TEST(UnicodeCharacterTest, SingleRuneRoundTripsDuringConstantEvaluation) {
    constexpr auto valid = [] {
        for (const auto rune : std::array<Rune, 4>{0, U'中', U'😀', 0x10FFFF}) {
            if (decode_one(encode_one<TypeParam>(rune)) != rune) {
                return false;
            }
        }
        return true;
    }();
    static_assert(valid);
    EXPECT_TRUE(valid);
}

TEST(UnicodeTest, MalformedUtf8ThrowsInsteadOfReturningEmptyOutput) {
    const auto inputs = std::array<std::string_view, 13>{"\x80",
                                                         "\xC0\x80",
                                                         "\xC1\xBF",
                                                         "\xE0\x80\x80",
                                                         "\xED\xA0\x80",
                                                         "\xF0\x80\x80\x80",
                                                         "\xF4\x90\x80\x80",
                                                         "\xF5\x80\x80\x80",
                                                         "\xFF",
                                                         "\xC2",
                                                         "\xE4\xBD",
                                                         "\xF0\x9F\x98",
                                                         "\xE4\x41\xA0"};
    for (const auto input : inputs) {
        EXPECT_FALSE(is_valid_utf(input));
        EXPECT_THROW(decode_one(input), LogConfig::Exception);
        EXPECT_THROW(decode(input), LogConfig::Exception);
        EXPECT_THROW(decode_with_offset(input), LogConfig::Exception);
    }
}

TEST(UnicodeTest, MalformedUtf16ThrowsInsteadOfReturningEmptyOutput) {
    const auto inputs =
        std::array{std::u16string{char16_t{0xD800}}, std::u16string{char16_t{0xDC00}},
                   std::u16string{char16_t{0xD800}, u'A'}, std::u16string{char16_t{0xDC00}, char16_t{0xD800}}};
    for (const auto &input : inputs) {
        EXPECT_FALSE(is_valid_utf(input));
        EXPECT_THROW(decode_one(input), LogConfig::Exception);
        EXPECT_THROW(decode(input), LogConfig::Exception);
        EXPECT_THROW(decode_with_offset(input), LogConfig::Exception);
    }
}

TEST(UnicodeTest, InvalidUtf32ScalarsThrowInAllDecoders) {
    for (const auto rune : std::array<Rune, 3>{0xD800, 0xDFFF, 0x110000}) {
        const auto input = std::u32string{rune};
        EXPECT_FALSE(is_valid_utf(input));
        EXPECT_THROW(decode_one(input), LogConfig::Exception);
        EXPECT_THROW(decode(input), LogConfig::Exception);
        EXPECT_THROW(decode_with_offset(input), LogConfig::Exception);
    }
}

TEST(UnicodeTest, UtfValidationAcceptsEmptyTextInEveryEncoding) {
    EXPECT_TRUE(is_valid_utf(std::string_view{}));
    EXPECT_TRUE(is_valid_utf(std::u8string_view{}));
    EXPECT_TRUE(is_valid_utf(std::u16string_view{}));
    EXPECT_TRUE(is_valid_utf(std::u32string_view{}));
    EXPECT_TRUE(is_valid_utf(std::wstring_view{}));
}

TEST(UnicodeTest, UtfValidationAcceptsEmbeddedNullAndSupplementaryCharacters) {
    EXPECT_TRUE(is_valid_utf(std::string_view{"A\0中😀", 9}));
    EXPECT_TRUE(is_valid_utf(std::u8string_view{u8"A\0中😀", 9}));
    EXPECT_TRUE(is_valid_utf(std::u16string_view{u"A\0中😀", 5}));
    EXPECT_TRUE(is_valid_utf(std::u32string_view{U"A\0中😀", 4}));
    EXPECT_TRUE(is_valid_utf(std::wstring_view{L"中😀"}));
}

TEST(UnicodeTest, UtfValidationChecksMalformedSuffixAfterEmbeddedNull) {
    EXPECT_FALSE(is_valid_utf(std::string_view{"A\0\x80", 3}));
    EXPECT_FALSE(is_valid_utf(std::u16string{u'A', u'\0', char16_t{0xD800}}));
    EXPECT_FALSE(is_valid_utf(std::u32string{U'A', U'\0', char32_t{0x110000}}));
}

TEST(UnicodeTest, Utf8CharacterTypeStillRequiresByteValidation) {
    const auto input = std::u8string{char8_t{0xFF}};
    EXPECT_FALSE(is_valid_utf(input));
    EXPECT_THROW(decode(input), LogConfig::Exception);
}

TEST(UnicodeTest, UtfValidationSupportsConstantEvaluation) {
    static_assert(is_valid_utf(std::string_view{"中😀"}));
    static_assert(is_valid_utf(std::u16string_view{u"中😀"}));
    static_assert(is_valid_utf(std::u32string_view{U"中😀"}));
    static_assert(!is_valid_utf(std::string_view{"\xC0\x80"}));
    static_assert(!is_valid_utf(std::string_view{"\xE4\xB8"}));
}

TEST(UnicodeTest, DecodeErrorReportsEncodingOffsetAndReason) {
    const auto input = std::string_view{"a\xE4\xBD"};
    try {
        (void)decode(input);
        FAIL() << "Expected a Unicode decoding exception";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find("UTF-8"), std::string_view::npos);
        EXPECT_NE(message.find("code-unit offset 1"), std::string_view::npos);
        EXPECT_NE(message.find("truncated sequence"), std::string_view::npos);
    }
}

namespace {

// An accepted fast decode must equal the full decoder's successful result; a declined one is always consistent.
auto fast_decode_agrees(std::span<const char> input) -> bool {
    const auto fast = decode_utf8_fast(input);
    if (fast.code_units == 0) {
        return true;
    }
    const auto full = decode_one_utf8(input);
    return full.error == UnicodeDecodeError::None && full.rune == fast.rune && full.code_units == fast.code_units;
}

// The decoding error message of input, or an empty string when input decodes.
auto decode_error_message(std::string_view input) -> std::string {
    try {
        (void)decode_with_offset(input);
    } catch (const LogConfig::Exception &error) {
        return error.what();
    }
    return {};
}

} // namespace

// The fast path reads continuation bytes only after an E0–EF lead, so other leads need one sample each.
TEST(UnicodeTest, Utf8FastPathAgreesWithFullDecoderWheneverItAccepts) {
    for (auto first = 0; first < 256; ++first) {
        const auto tails = (first & 0xF0) == 0xE0 ? 256 : 1;
        for (auto second = 0; second < tails; ++second) {
            for (auto third = 0; third < tails; ++third) {
                const auto bytes = std::array{static_cast<char>(first), static_cast<char>(tails == 1 ? 0x80 : second),
                                              static_cast<char>(tails == 1 ? 0x80 : third)};
                ASSERT_TRUE(fast_decode_agrees(bytes)) << first << ' ' << second << ' ' << third;
            }
        }
    }
}

TEST(UnicodeTest, Utf8FastPathAcceptsAsciiAndCommonThreeByteSequences) {
    for (auto ascii = 0; ascii < 0x80; ++ascii) {
        const auto bytes = std::array{static_cast<char>(ascii)};
        ASSERT_EQ(decode_utf8_fast(std::span<const char>{bytes}).code_units, 1) << ascii;
    }
    for (auto first = 0xE1; first <= 0xEF; ++first) {
        if (first == 0xED) {
            continue; // surrogate range: left to the full decoder
        }
        for (auto second = 0x80; second <= 0xBF; ++second) {
            for (auto third = 0x80; third <= 0xBF; ++third) {
                const auto bytes =
                    std::array{static_cast<char>(first), static_cast<char>(second), static_cast<char>(third)};
                ASSERT_EQ(decode_utf8_fast(std::span<const char>{bytes}).code_units, 3) << first << ' ' << second;
            }
        }
    }
}

// Overlong, surrogate, two- and four-byte, truncated and malformed sequences need the full decoder's checks.
TEST(UnicodeTest, Utf8FastPathLeavesOtherSequencesToFullDecoder) {
    const auto inputs =
        std::array<std::string_view, 8>{"\xE0\xA0\x80",     "\xED\x9F\xBF", "\xED\xA0\x80", "\xC2\xA9",
                                        "\xF0\x9F\x98\x80", "\xE4\xB8",     "\x80",         "\xE4\x41\xA0"};
    for (const auto input : inputs) {
        EXPECT_EQ(decode_utf8_fast(std::span<const char>{input}).code_units, 0) << input.size();
    }
}

TEST(UnicodeTest, DecodeErrorAfterFastPathRunesKeepsOffsetAndReason) {
    const auto invalid_lead = decode_error_message("中文\xFF");
    EXPECT_NE(invalid_lead.find("code-unit offset 6"), std::string::npos) << invalid_lead;
    EXPECT_NE(invalid_lead.find("invalid leading code unit"), std::string::npos) << invalid_lead;
    const auto surrogate = decode_error_message("中\xED\xA0\x80");
    EXPECT_NE(surrogate.find("code-unit offset 3"), std::string::npos) << surrogate;
    EXPECT_NE(surrogate.find("surrogate code point"), std::string::npos) << surrogate;
}

TEST(UnicodeTest, DecodeOneRejectsEmptyInputButAcceptsNul) {
    EXPECT_THROW(decode_one(std::string_view{}), LogConfig::Exception);
    EXPECT_THROW(decode_one(std::u16string_view{}), LogConfig::Exception);
    EXPECT_THROW(decode_one(std::u32string_view{}), LogConfig::Exception);
    EXPECT_EQ(decode_one(std::string_view{"\0", 1}), Rune{});
    EXPECT_TRUE(decode(std::string_view{}).empty());
}

TEST(UnicodeTest, OwningTemporaryCanBeDecoded) {
    EXPECT_EQ(decode(std::string{"你好"}), (Unicode{U'你', U'好'}));
}

TEST(UnicodeTest, DecodedStorageCannotBeBorrowedFromTemporaries) {
    static_assert(CanBorrowRunes<UnicodeWithOffset &> && CanBorrowRunes<const UnicodeWithOffset &>);
    static_assert(CanBorrowOffsets<UnicodeWithOffset &> && CanBorrowOffsets<const UnicodeWithOffset &>);
    static_assert(!CanBorrowRunes<UnicodeWithOffset> && !CanBorrowRunes<const UnicodeWithOffset>);
    static_assert(!CanBorrowOffsets<UnicodeWithOffset> && !CanBorrowOffsets<const UnicodeWithOffset>);
}

TEST(UnicodeTest, SourceEncodingRejectsInvalidRuneRanges) {
    const auto source = std::string_view{"abc"};
    const auto offsets = std::array<SourceOffset, 4>{0, 1, 2, 3};
    EXPECT_THROW(encode(source, offsets, WordRange{2, 1}), LogConfig::Exception);
    EXPECT_THROW(encode(source, offsets, WordRange{0, 4}), LogConfig::Exception);
    EXPECT_THROW(encode(source, std::span<const SourceOffset>{}, WordRange{0, 0}), LogConfig::Exception);
}

TEST(UnicodeTest, LongUtf8SourcePreservesDistinctRuneAndCodeUnitOffsets) {
    const auto prefix = std::string(65'536, 'a');
    const auto source = prefix + "你𠮷";
    const auto decoded = decode_with_offset(source);
    const auto begin = static_cast<RuneIndex>(prefix.size());
    const auto end = static_cast<RuneIndex>(decoded.runes.size());
    EXPECT_EQ(decoded.runes.size(), prefix.size() + 2);
    EXPECT_EQ(decoded.offsets[begin], static_cast<SourceOffset>(prefix.size()));
    EXPECT_EQ(decoded.offsets[end], static_cast<SourceOffset>(source.size()));
    EXPECT_EQ(encode(std::string_view{source}, decoded.offsets, WordRange{begin, end}), "你𠮷");
}

TEST(UnicodeTest, SourceEncodingRejectsInvalidCodeUnitOffsets) {
    const auto source = std::string_view{"abc"};
    const auto reversed = std::array<SourceOffset, 2>{2, 1};
    const auto oversized = std::array<SourceOffset, 2>{0, 4};
    EXPECT_THROW(encode(source, reversed, WordRange{0, 1}), LogConfig::Exception);
    EXPECT_THROW(encode(source, oversized, WordRange{0, 1}), LogConfig::Exception);
}

TEST(UnicodeTest, EmptySourceCanBeEncodedWithItsSentinel) {
    const auto offsets = std::array<SourceOffset, 1>{0};
    EXPECT_TRUE(encode(std::string_view{}, offsets, WordRange{0, 0}).empty());
}

// ─── decode_with_source: UTF-8 ──────────────────────────────────────────────

TEST(UnicodeWithSourceTest, Utf8Ascii) {
    auto input = std::string{"Hello"};
    auto result = decode_with_offset(input);

    ASSERT_EQ(result.runes.size(), 5u);
    ASSERT_EQ(result.offsets.size(), 6u); // runes.size() + 1
    EXPECT_EQ(result.runes[0], U'H');
    EXPECT_EQ(result.runes[4], U'o');

    // All ASCII: each rune is 1 byte
    for (size_t i = 0; i <= 5; ++i) {
        EXPECT_EQ(result.offsets[i], i);
    }
}

TEST(UnicodeWithSourceTest, Utf8Chinese) {
    auto input = std::string{"你好世界"};
    auto result = decode_with_offset(input);

    ASSERT_EQ(result.runes.size(), 4u);
    ASSERT_EQ(result.offsets.size(), 5u);

    EXPECT_EQ(result.runes[0], U'你');
    EXPECT_EQ(result.runes[1], U'好');
    EXPECT_EQ(result.runes[2], U'世');
    EXPECT_EQ(result.runes[3], U'界');

    // Each Chinese character is 3 bytes in UTF-8
    EXPECT_EQ(result.offsets[0], 0u);
    EXPECT_EQ(result.offsets[1], 3u);
    EXPECT_EQ(result.offsets[2], 6u);
    EXPECT_EQ(result.offsets[3], 9u);
    EXPECT_EQ(result.offsets[4], 12u);
}

TEST(UnicodeWithSourceTest, Utf8Mixed) {
    // Mix of ASCII and multi-byte characters
    auto input = std::string{"a你b好c"};
    auto result = decode_with_offset(input);

    ASSERT_EQ(result.runes.size(), 5u);
    EXPECT_EQ(result.runes[0], U'a');
    EXPECT_EQ(result.runes[1], U'你');
    EXPECT_EQ(result.runes[2], U'b');
    EXPECT_EQ(result.runes[3], U'好');
    EXPECT_EQ(result.runes[4], U'c');

    // offsets: a(1) 你(3) b(1) 好(3) c(1) = total 9 bytes
    EXPECT_EQ(result.offsets[0], 0u);
    EXPECT_EQ(result.offsets[1], 1u);
    EXPECT_EQ(result.offsets[2], 4u);
    EXPECT_EQ(result.offsets[3], 5u);
    EXPECT_EQ(result.offsets[4], 8u);
    EXPECT_EQ(result.offsets[5], 9u);
}

TEST(UnicodeWithSourceTest, Utf8FourByte) {
    // Emoji: 😀 = U+1F600 = 4 bytes in UTF-8
    auto input = std::string{"😀"};
    auto result = decode_with_offset(input);

    ASSERT_EQ(result.runes.size(), 1u);
    EXPECT_EQ(result.runes[0], U'\U0001F600');
    EXPECT_EQ(result.offsets[0], 0u);
    EXPECT_EQ(result.offsets[1], 4u);
}

TEST(UnicodeWithSourceTest, Utf8Empty) {
    auto input = std::string{};
    auto result = decode_with_offset(input);

    EXPECT_TRUE(result.runes.empty());
    ASSERT_EQ(result.offsets.size(), 1u);
    EXPECT_EQ(result.offsets[0], 0u);
}

// ─── encode_range ───────────────────────────────────────────────────────────

TEST(UnicodeWithSourceTest, EncodeRangeFull) {
    auto input = std::string{"你好世界"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);

    const auto &runes = result.get_runes();
    const auto &offsets = result.get_offsets();

    auto range = WordRange{0, static_cast<RuneIndex>(runes.size())};
    auto encoded = encode(view, offsets, range);

    EXPECT_EQ(encoded, input);
}

TEST(UnicodeWithSourceTest, EncodeRangeSubstring) {
    auto input = std::string{"你好世界"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);

    const auto &offsets = result.get_offsets();

    // Encode just "好世"
    auto range = WordRange{1, 3};
    auto encoded = encode(view, offsets, range);
    EXPECT_EQ(encoded, "好世");
}

TEST(UnicodeWithSourceTest, EncodeRangeEmpty) {
    auto input = std::string{"你好"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);
    const auto &offsets = result.get_offsets();

    auto range = WordRange{0, 0};
    auto encoded = encode(view, offsets, range);
    EXPECT_TRUE(encoded.empty());
}

TEST(UnicodeWithSourceTest, EncodeAll) {
    auto input = std::string{"Hello 你好"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);
    const auto &offsets = result.get_offsets();

    auto range = WordRange{0, static_cast<RuneIndex>(result.get_runes().size())};
    auto encoded = encode(view, offsets, range);
    EXPECT_EQ(encoded, input);
}

// ─── source_of ──────────────────────────────────────────────────────────────

TEST(UnicodeWithSourceTest, SourceOf) {
    auto input = std::string{"a你b"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);
    const auto &offsets = result.get_offsets();

    EXPECT_EQ(encode(view, offsets, WordRange{0, 1}), "a");
    EXPECT_EQ(encode(view, offsets, WordRange{1, 2}), "你");
    EXPECT_EQ(encode(view, offsets, WordRange{2, 3}), "b");
}

// ─── Roundtrip: decode_with_source + encode_range == normal decode + encode ─

TEST(UnicodeWithSourceTest, RoundtripMatchesNormalEncode) {
    auto input = std::string{"南京市长江大桥欢迎您！Hello, world! 😀🌍"};
    auto sourced = decode_with_offset(input);
    auto normal = decode(input);

    // Runes must match
    ASSERT_EQ(sourced.runes.size(), normal.size());
    for (size_t i = 0; i < normal.size(); ++i) {
        EXPECT_EQ(sourced.runes[i], normal[i]);
    }

    // Full encode must match
    auto view = as_view(input);
    const auto &offsets = sourced.get_offsets();
    auto fast_encoded = encode(view, offsets, WordRange{0, static_cast<RuneIndex>(sourced.get_runes().size())});
    auto normal_encoded = encode(std::span<const Rune>(normal));
    EXPECT_EQ(fast_encoded, normal_encoded);
    EXPECT_EQ(fast_encoded, input);

    // Sub-range encode must match for various ranges
    for (size_t start = 0; start < normal.size(); ++start) {
        for (size_t count = 0; count + start <= normal.size() && count <= 5; ++count) {
            auto fast =
                encode(view, offsets, WordRange{static_cast<RuneIndex>(start), static_cast<RuneIndex>(start + count)});
            auto slow = encode(std::span<const Rune>(normal.data() + start, count));
            EXPECT_EQ(fast, slow) << "Mismatch at start=" << start << " count=" << count;
        }
    }
}

// ─── UTF-16 ─────────────────────────────────────────────────────────────────

TEST(UnicodeWithSourceTest, Utf16Basic) {
    auto input = std::u16string{u"你好世界"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);
    const auto &offsets = result.get_offsets();

    ASSERT_EQ(result.runes.size(), 4u);
    EXPECT_EQ(result.runes[0], U'你');
    EXPECT_EQ(result.runes[3], U'界');

    // Each CJK character is 1 code unit in UTF-16
    for (size_t i = 0; i <= 4; ++i) {
        EXPECT_EQ(result.offsets[i], i);
    }

    auto encoded = encode(view, offsets, WordRange{0, static_cast<RuneIndex>(result.get_runes().size())});
    EXPECT_EQ(encoded, input);
    EXPECT_EQ(encode(view, offsets, WordRange{1, 3}), u"好世");
}

TEST(UnicodeWithSourceTest, Utf16Surrogate) {
    // 😀 = U+1F600, requires surrogate pair in UTF-16
    auto input = std::u16string{u"😀"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);
    const auto &offsets = result.get_offsets();

    ASSERT_EQ(result.runes.size(), 1u);
    EXPECT_EQ(result.runes[0], U'\U0001F600');
    EXPECT_EQ(result.offsets[0], 0u);
    EXPECT_EQ(result.offsets[1], 2u); // 2 code units for surrogate pair

    auto encoded = encode(view, offsets, WordRange{0, static_cast<RuneIndex>(result.get_runes().size())});
    EXPECT_EQ(encoded, input);
}

// ─── UTF-32 ─────────────────────────────────────────────────────────────────

TEST(UnicodeWithSourceTest, Utf32Basic) {
    auto input = std::u32string{U"你好世界"};
    auto result = decode_with_offset(input);

    auto view = as_view(input);
    const auto &offsets = result.get_offsets();

    ASSERT_EQ(result.runes.size(), 4u);
    EXPECT_EQ(result.runes[0], U'你');
    EXPECT_EQ(result.runes[3], U'界');

    // Each character is 1 code unit in UTF-32
    for (size_t i = 0; i <= 4; ++i) {
        EXPECT_EQ(result.offsets[i], i);
    }

    auto encoded = encode(view, offsets, WordRange{0, static_cast<RuneIndex>(result.get_runes().size())});
    EXPECT_EQ(encoded, input);
    EXPECT_EQ(encode(view, offsets, WordRange{1, 3}), U"好世");
}

// ─── Consistency: decode_with_source runes == decode runes ──────────────────

TEST(UnicodeWithSourceTest, ConsistencyWithDecode) {
    auto inputs = std::vector<std::string>{
        "", "ASCII only", "你好世界", "Mixed 混合 text 文本 123", "😀🌍🎉", "emoji 😀 in 中间 of text",
    };

    for (const auto &input : inputs) {
        auto normal = decode(input);
        auto sourced = decode_with_offset(input);

        ASSERT_EQ(sourced.runes.size(), normal.size()) << "Input: " << input;
        for (size_t i = 0; i < normal.size(); ++i) {
            EXPECT_EQ(sourced.runes[i], normal[i]) << "Input: " << input << " index: " << i;
        }
    }
}

TEST(UtfFacadeTest, DecodeAndEncodeRoundTripEveryEncoding) {
    const auto runes = neo_cppjieba::Utf::decode(std::string_view{"中文abc𠮷"});
    EXPECT_EQ(runes, (std::vector<Rune>{U'中', U'文', U'a', U'b', U'c', U'𠮷'}));
    EXPECT_EQ(neo_cppjieba::Utf::encode<char>(runes), "中文abc𠮷");
    EXPECT_EQ(neo_cppjieba::Utf::encode<char16_t>(runes), u"中文abc𠮷");
    EXPECT_EQ(neo_cppjieba::Utf::encode<char32_t>(runes), U"中文abc𠮷");
    EXPECT_EQ(neo_cppjieba::Utf::decode(std::u16string_view{u"中文abc𠮷"}), runes);
}

TEST(UtfFacadeTest, EmptyInputDecodesToNoRunes) {
    EXPECT_TRUE(neo_cppjieba::Utf::decode(std::string_view{}).empty());
    EXPECT_TRUE(neo_cppjieba::Utf::encode<char>(std::span<const Rune>{}).empty());
    EXPECT_TRUE(neo_cppjieba::Utf::is_valid(std::string_view{}));
}

TEST(UtfFacadeTest, MalformedInputIsInvalidAndDecodeThrows) {
    const auto malformed = std::string_view{"a\xE4\xB8"};
    EXPECT_FALSE(neo_cppjieba::Utf::is_valid(malformed));
    EXPECT_THROW(static_cast<void>(neo_cppjieba::Utf::decode(malformed)), LogConfig::Exception);
    EXPECT_FALSE(neo_cppjieba::Utf::is_valid(std::u16string_view{u"\xD800"}));
}

TEST(UtfFacadeTest, EmbeddedNulIsKept) {
    const auto text = std::string_view{"a\0b", 3};
    EXPECT_TRUE(neo_cppjieba::Utf::is_valid(text));
    EXPECT_EQ(neo_cppjieba::Utf::decode(text), (std::vector<Rune>{U'a', U'\0', U'b'}));
}

static_assert(!std::is_default_constructible_v<neo_cppjieba::Utf>);
