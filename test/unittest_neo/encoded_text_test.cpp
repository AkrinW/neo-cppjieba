#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"
#include "neo/encoding/IcuCodec.hpp"
#include "neo/util/Unicode.hpp"

#include "test_paths.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;
using namespace std::string_view_literals;

// Source encodings belong to conversion; the segmentation facade accepts only its UTF input families.
static_assert(!std::default_initializable<IcuCodec>);

namespace {

constexpr auto gbk_academy = "\xD6\xD0\xB9\xFA\xBF\xC6\xD1\xA7\xD4\xBA"sv;
constexpr auto encodings = std::array{EncodingId::UTF8, EncodingId::GBK, EncodingId::GB18030};

// Keep each source encoding and its expected UTF-8 text in the same test case.
struct ConversionCase {
    std::string_view bytes;
    EncodingId encoding;
    std::string_view expected_utf8;
};

// Reuse immutable dictionaries while exercising the encoded input facade.
// The conversion step now precedes the ordinary UTF facade.
auto test_jieba() -> const Jieba & {
    static const auto jieba = Jieba{DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8", ""};
    return jieba;
}

// Compare source-encoded tokens with the UTF-8 expectations used by existing segmentation tests.
// The result already contains UTF-8 after outer conversion.
template <typename Result>
auto utf8_words(const Result &result) -> std::vector<std::string> {
    auto words = std::vector<std::string>{};
    words.reserve(result.size());
    for (const auto token : result) {
        words.emplace_back(token.word);
    }
    return words;
}

} // namespace

TEST(IcuCodecTest, GbkOffsetsReferToOriginalBytes) {
    auto decoded = UnicodeWithOffset{};
    IcuCodec::decode_with_offset_into("\x41\xD6\xD0\x42"sv, EncodingId::GBK, decoded);
    EXPECT_EQ(decoded.runes, (detail::Unicode{U'A', U'中', U'B'}));
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0, 1, 3, 4}));
}

TEST(IcuCodecTest, Gb18030FourByteCharacterProducesOneRune) {
    auto decoded = UnicodeWithOffset{};
    IcuCodec::decode_with_offset_into("\x41\x94\x39\xFC\x36\x5A"sv, EncodingId::GB18030, decoded);
    EXPECT_EQ(decoded.runes, (detail::Unicode{U'A', U'😀', U'Z'}));
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0, 1, 5, 6}));
}

TEST(IcuCodecTest, EmbeddedNullDoesNotTerminateInput) {
    const auto input = "A\0\xD6\xD0"sv;
    auto decoded = UnicodeWithOffset{};
    IcuCodec::decode_with_offset_into(input, EncodingId::GBK, decoded);
    EXPECT_EQ(decoded.runes, (detail::Unicode{U'A', U'\0', U'中'}));
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0, 1, 2, 4}));
    EXPECT_EQ(IcuCodec::to_utf8(input, EncodingId::GBK), std::string("A\0中", 5));
}

TEST(IcuCodecTest, EmptyInputReplacesPreviousOutputWithZeroSentinel) {
    auto decoded = detail::decode_with_offset("旧内容");
    for (const auto encoding : encodings) {
        IcuCodec::decode_with_offset_into(std::string_view{}, encoding, decoded);
        EXPECT_TRUE(decoded.runes.empty());
        EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0}));
        EXPECT_TRUE(IcuCodec::to_utf8(std::string_view{}, encoding).empty());
    }
}

TEST(IcuCodecTest, MalformedSequencesRaiseErrorsWithoutReplacement) {
    auto decoded = UnicodeWithOffset{};
    const auto expect_invalid = [&](std::string_view input, EncodingId encoding) {
        EXPECT_THROW(IcuCodec::decode_with_offset_into(input, encoding, decoded), LogConfig::Exception);
        EXPECT_THROW(static_cast<void>(IcuCodec::to_utf8(input, encoding)), LogConfig::Exception);
    };
    expect_invalid("\xFF"sv, EncodingId::UTF8);
    expect_invalid("\x81"sv, EncodingId::GBK);
    expect_invalid("\x81\x30"sv, EncodingId::GBK);
    expect_invalid("\x94\x39\xFC\x36"sv, EncodingId::GBK);
    expect_invalid("\x81\x30\x81"sv, EncodingId::GB18030);
    expect_invalid("\x81\x30\x20\x30"sv, EncodingId::GB18030);
}

TEST(IcuCodecTest, MalformedInputReportsEncodingAndOriginalByteOffset) {
    auto decoded = UnicodeWithOffset{};
    try {
        IcuCodec::decode_with_offset_into("A\x81\x30"sv, EncodingId::GBK, decoded);
        FAIL() << "Expected strict GBK decoding to fail";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find("GBK"), std::string_view::npos);
        EXPECT_NE(message.find("byte offset 1"), std::string_view::npos);
    }
}

TEST(IcuCodecTest, BuffersCanBeReusedAfterDecodeFailure) {
    auto decoded = UnicodeWithOffset{};
    EXPECT_THROW(IcuCodec::decode_with_offset_into("A\x81"sv, EncodingId::GBK, decoded), LogConfig::Exception);
    IcuCodec::decode_with_offset_into("\xD6\xD0"sv, EncodingId::GBK, decoded);
    EXPECT_EQ(decoded.runes, (detail::Unicode{U'中'}));
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0, 2}));
}

TEST(IcuCodecTest, ExplicitUtf8PreservesNativeOffsetsAndBom) {
    const auto source = "\xEF\xBB\xBF中😀"sv;
    const auto expected = detail::decode_with_offset(source);
    auto decoded = UnicodeWithOffset{};
    IcuCodec::decode_with_offset_into(source, EncodingId::UTF8, decoded);
    EXPECT_EQ(decoded.runes, expected.runes);
    EXPECT_EQ(decoded.offsets, expected.offsets);
    EXPECT_EQ(IcuCodec::to_utf8(source, EncodingId::UTF8), source);
}

TEST(IcuCodecTest, TemporaryByteStringProducesOwnedUtf8) {
    const auto converted = IcuCodec::to_utf8(std::string{gbk_academy}, EncodingId::GBK);
    EXPECT_EQ(converted, "中国科学院");
}

TEST(IcuCodecTest, TemporaryByteStringProducesOwnedRunesAndOffsets) {
    auto decoded = UnicodeWithOffset{};
    IcuCodec::decode_with_offset_into(std::string{gbk_academy}, EncodingId::GBK, decoded);
    EXPECT_EQ(decoded.runes, Utf::decode("中国科学院"));
    EXPECT_EQ(decoded.offsets, (std::vector<SourceOffset>{0, 2, 4, 6, 8, 10}));
}

TEST(IcuCodecTest, EncodesUtf8AsGbk) {
    EXPECT_EQ(IcuCodec::from_utf8("中国科学院"sv, EncodingId::GBK), gbk_academy);
}

TEST(IcuCodecTest, EncodedOffsetsReferToTargetBytes) {
    const auto encoded = IcuCodec::from_utf8_with_offset("A中B"sv, EncodingId::GBK);
    EXPECT_EQ(encoded.bytes, "\x41\xD6\xD0\x42"sv);
    EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0, 1, 3, 4}));
    EXPECT_EQ(encoded.bytes, IcuCodec::from_utf8("A中B"sv, EncodingId::GBK));
}

TEST(IcuCodecTest, SupplementaryScalarHasOneEncodedBoundary) {
    const auto encoded = IcuCodec::from_utf8_with_offset("A😀Z"sv, EncodingId::GB18030);
    EXPECT_EQ(encoded.bytes, "\x41\x94\x39\xFC\x36\x5A"sv);
    EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0, 1, 5, 6}));
    EXPECT_EQ(encoded.bytes, IcuCodec::from_utf8("A😀Z"sv, EncodingId::GB18030));
    EXPECT_EQ(IcuCodec::to_utf8(encoded.bytes, EncodingId::GB18030), "A😀Z");
}

TEST(IcuCodecTest, Gb18030OffsetsTrackExpansionBeyondUtf8Length) {
    const auto encoded = IcuCodec::from_utf8_with_offset("A\u0080B"sv, EncodingId::GB18030);
    EXPECT_EQ(encoded.bytes, "\x41\x81\x30\x81\x30\x42"sv);
    EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0, 1, 5, 6}));
}

TEST(IcuCodecTest, EncodingPreservesEmbeddedNull) {
    const auto encoded = IcuCodec::from_utf8_with_offset("A\0中"sv, EncodingId::GBK);
    EXPECT_EQ(encoded.bytes, "A\0\xD6\xD0"sv);
    EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0, 1, 2, 4}));
    EXPECT_EQ(encoded.bytes, IcuCodec::from_utf8("A\0中"sv, EncodingId::GBK));
}

TEST(IcuCodecTest, EmptyEncodingProducesZeroSentinel) {
    for (const auto encoding : encodings) {
        EXPECT_TRUE(IcuCodec::from_utf8(std::string_view{}, encoding).empty());
        const auto encoded = IcuCodec::from_utf8_with_offset(std::string_view{}, encoding);
        EXPECT_TRUE(encoded.bytes.empty());
        EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0}));
    }
}

TEST(IcuCodecTest, MalformedUtf8CannotBeEncoded) {
    const auto malformed =
        std::array{"\xFF"sv, "A\xC0\x80"sv, "\xED\xA0\x80"sv, "\xF4\x90\x80\x80"sv, "Z\xF0\x9F\x98"sv, "A\0\xFF"sv};
    for (const auto encoding : encodings) {
        for (const auto input : malformed) {
            EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8(input, encoding)), LogConfig::Exception);
            EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8_with_offset(input, encoding)), LogConfig::Exception);
        }
    }
}

TEST(IcuCodecTest, UnrepresentableScalarsRaiseErrorsWithoutSubstitution) {
    EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8("中A😀"sv, EncodingId::GBK)), LogConfig::Exception);
    EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8_with_offset("中A😀"sv, EncodingId::GBK)), LogConfig::Exception);
}

TEST(IcuCodecTest, UnrepresentableDefaultIgnorableIsNotSkipped) {
    EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8("A\u200DB"sv, EncodingId::GBK)), LogConfig::Exception);
    EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8_with_offset("A\u200DB"sv, EncodingId::GBK)),
                 LogConfig::Exception);
}

TEST(IcuCodecTest, SkippedDefaultIgnorableReportsScalarAndUtf8ByteOffset) {
    try {
        const auto encoded = IcuCodec::from_utf8("A\u200DB"sv, EncodingId::GBK);
        FAIL() << "Expected GBK encoding to fail, got " << encoded.size() << " bytes";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find("GBK"), std::string_view::npos);
        EXPECT_NE(message.find("U+200D"), std::string_view::npos);
        EXPECT_NE(message.find("UTF-8 byte offset 1"), std::string_view::npos);
    }
}

TEST(IcuCodecTest, RepresentableDefaultIgnorableIsEncoded) {
    const auto input = "A\u200DB"sv;
    const auto encoded = IcuCodec::from_utf8_with_offset(input, EncodingId::GB18030);
    EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0, 1, 5, 6}));
    EXPECT_EQ(IcuCodec::to_utf8(encoded.bytes, EncodingId::GB18030), input);
}

TEST(IcuCodecTest, EncodingFailureReportsTargetAndUtf8ByteOffset) {
    try {
        const auto encoded = IcuCodec::from_utf8("中A😀"sv, EncodingId::GBK);
        FAIL() << "Expected GBK encoding to fail, got " << encoded.size() << " bytes";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find("GBK"), std::string_view::npos);
        EXPECT_NE(message.find("U+1F600"), std::string_view::npos);
        EXPECT_NE(message.find("UTF-8 byte offset 4"), std::string_view::npos);
    }
}

TEST(IcuCodecTest, Utf8TargetValidatesAndPreservesBytesAndOffsets) {
    const auto input = "\xEF\xBB\xBF中😀\0Z"sv;
    const auto encoded = IcuCodec::from_utf8_with_offset(input, EncodingId::UTF8);
    EXPECT_EQ(encoded.bytes, input);
    EXPECT_EQ(encoded.offsets, detail::decode_with_offset(input).offsets);
    EXPECT_EQ(IcuCodec::from_utf8(input, EncodingId::UTF8), input);
}

TEST(IcuCodecTest, TemporaryUtf8ProducesOwnedEncodedBytesAndOffsets) {
    const auto encoded = IcuCodec::from_utf8_with_offset(std::string{"中国科学院"}, EncodingId::GBK);
    EXPECT_EQ(encoded.bytes, gbk_academy);
    EXPECT_EQ(encoded.offsets, (std::vector<SourceOffset>{0, 2, 4, 6, 8, 10}));
    EXPECT_EQ(IcuCodec::from_utf8(std::string{"中国科学院"}, EncodingId::GBK), gbk_academy);
}

TEST(IcuCodecTest, EncodingAfterFailureSucceeds) {
    EXPECT_THROW(static_cast<void>(IcuCodec::from_utf8("😀"sv, EncodingId::GBK)), LogConfig::Exception);
    EXPECT_EQ(IcuCodec::from_utf8("中国科学院"sv, EncodingId::GBK), gbk_academy);
}

TEST(EncodedJiebaTest, RuneRangesSliceOriginalGbkSearchTokens) {
    auto decoded = UnicodeWithOffset{};
    IcuCodec::decode_with_offset_into(gbk_academy, EncodingId::GBK, decoded);
    const auto utf8 = Utf::encode<char>(decoded.runes);
    const auto result = test_jieba().cut(utf8, CutMode::SEARCH);
    const auto expected = std::array{gbk_academy.substr(0, 4), gbk_academy.substr(4, 4), gbk_academy.substr(6, 4),
                                     gbk_academy.substr(4, 6), gbk_academy};
    ASSERT_EQ(result.size(), expected.size());
    for (size_t i = 0; i < result.size(); ++i) {
        const auto range = result[i].position.runes;
        const auto begin = decoded.offsets[range.begin];
        const auto end = decoded.offsets[range.end];
        EXPECT_EQ(gbk_academy.substr(begin, end - begin), expected[i]);
    }
}

TEST(EncodedJiebaTest, RuneRangesSliceNewlyEncodedGbkSearchTokens) {
    const auto utf8 = "中国科学院"sv;
    const auto result = test_jieba().cut(utf8, CutMode::SEARCH);
    const auto encoded = IcuCodec::from_utf8_with_offset(utf8, EncodingId::GBK);
    const auto expected = std::array{gbk_academy.substr(0, 4), gbk_academy.substr(4, 4), gbk_academy.substr(6, 4),
                                     gbk_academy.substr(4, 6), gbk_academy};
    ASSERT_EQ(result.size(), expected.size());
    for (size_t i = 0; i < result.size(); ++i) {
        const auto range = result[i].position.runes;
        const auto begin = encoded.offsets[range.begin];
        const auto end = encoded.offsets[range.end];
        EXPECT_EQ(std::string_view{encoded.bytes}.substr(begin, end - begin), expected[i]);
    }
}

TEST(EncodedJiebaTest, ConvertedTokensBorrowUtf8Storage) {
    const auto source = std::string{gbk_academy};
    const auto utf8 = IcuCodec::to_utf8(source, EncodingId::GBK);
    const auto result = test_jieba().cut(utf8, CutMode::MIX);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].word, "中国科学院");
    EXPECT_EQ(result[0].word.data(), utf8.data());
    EXPECT_EQ(result[0].position, (TokenPosition{{0, 5}, {0, 15}}));
}

TEST(EncodedJiebaTest, ConvertedSearchTokensUseUtf8ByteRanges) {
    const auto utf8 = IcuCodec::to_utf8(gbk_academy, EncodingId::GBK);
    const auto result = test_jieba().cut(utf8, CutMode::SEARCH);
    EXPECT_EQ(utf8_words(result), (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
    const auto positions = result.positions();
    EXPECT_EQ((std::vector<TokenPosition>{positions.begin(), positions.end()}),
              (std::vector<TokenPosition>{
                  {{0, 2}, {0, 6}}, {{2, 4}, {6, 12}}, {{3, 5}, {9, 15}}, {{2, 5}, {6, 15}}, {{0, 5}, {0, 15}}}));
}

TEST(EncodedJiebaTest, EveryModeMatchesNativeUtf8AfterConversion) {
    const auto modes = std::array{CutMode::MIX,           CutMode::MIX_NO_HMM, CutMode::FULL, CutMode::SEARCH,
                                  CutMode::SEARCH_NO_HMM, CutMode::HMM,        CutMode::MP};
    const auto cases =
        std::array{ConversionCase{"A \xD6\xD0\xB9\xFA\xBF\xC6\xD1\xA7\xD4\xBA!"sv, EncodingId::GBK, "A 中国科学院!"sv},
                   ConversionCase{"\xD6\xD0\xB9\xFA\x94\x39\xFC\x36!"sv, EncodingId::GB18030, "中国😀!"sv}};
    for (const auto &entry : cases) {
        const auto utf8 = IcuCodec::to_utf8(entry.bytes, entry.encoding);
        EXPECT_EQ(utf8, entry.expected_utf8);
        for (const auto mode : modes) {
            const auto expected = test_jieba().cut(entry.expected_utf8, mode);
            const auto result = test_jieba().cut(utf8, mode);
            ASSERT_EQ(result.size(), expected.size());
            for (size_t i = 0; i < expected.size(); ++i) {
                EXPECT_EQ(result[i].word, expected[i].word);
                EXPECT_EQ(result[i].position, expected[i].position);
            }
        }
    }
}

TEST(EncodedJiebaTest, UnknownWordsKeepExistingNoHmmBoundaries) {
    const auto input = "\xCB\xFB\xC0\xB4\xB5\xBD\xC1\xCB\xCD\xF8\xD2\xD7\xBA\xBC\xD1\xD0\xB4\xF3\xCF\xC3"sv;
    const auto result = test_jieba().cut_owned(IcuCodec::to_utf8(input, EncodingId::GBK), CutMode::MIX_NO_HMM);
    EXPECT_EQ(join(utf8_words(result), "/"), "他/来到/了/网易/杭/研/大厦");
}

TEST(EncodedJiebaTest, ConvertingSourceSubviewProducesLocalUtf8Offsets) {
    const auto storage = std::string{"prefix"} + std::string{gbk_academy} + "suffix";
    const auto source = std::string_view{storage}.substr(6, gbk_academy.size());
    const auto result = test_jieba().cut_owned(IcuCodec::to_utf8(source, EncodingId::GBK), CutMode::MIX);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].word, "中国科学院");
    EXPECT_EQ(result[0].word.data(), result.source().data());
    EXPECT_EQ(result[0].position.source, (SourceRange{0, 15}));
}

TEST(EncodedJiebaTest, ResultsSurviveWorkspaceReuseReleaseAndDestruction) {
    const auto utf8 = IcuCodec::to_utf8(gbk_academy, EncodingId::GBK);
    const auto result = [&] {
        auto workspace = Workspace{};
        auto first = test_jieba().cut_with_workspace(utf8, CutMode::SEARCH, workspace);
        const auto second = test_jieba().cut_with_workspace("另一段文字"sv, CutMode::MIX, workspace);
        workspace.release();
        EXPECT_FALSE(second.empty());
        return first;
    }();
    EXPECT_EQ(utf8_words(result), (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
    EXPECT_EQ(result.source().data(), utf8.data());
}

TEST(EncodedJiebaTest, EmptyConvertedInputProducesNoTokens) {
    for (const auto encoding : encodings) {
        const auto result = test_jieba().cut_owned(IcuCodec::to_utf8(std::string_view{}, encoding), CutMode::MIX);
        EXPECT_TRUE(result.empty());
    }
}

TEST(EncodedJiebaTest, ValidUtf8BytesDoNotIdentifyTheirOriginalEncoding) {
    const auto bytes = "\xC2\xA9"sv;
    ASSERT_TRUE(Utf::is_valid(bytes));
    const auto as_utf8 = IcuCodec::to_utf8(bytes, EncodingId::UTF8);
    const auto as_gbk = IcuCodec::to_utf8(bytes, EncodingId::GBK);
    EXPECT_EQ(as_utf8, "©");
    EXPECT_NE(as_gbk, as_utf8);
    EXPECT_TRUE(Utf::is_valid(as_gbk));
}

TEST(EncodedJiebaTest, Utf8ConversionCanFeedExistingOwnedResults) {
    const auto result = test_jieba().cut_owned(IcuCodec::to_utf8(gbk_academy, EncodingId::GBK), CutMode::MIX);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].word, "中国科学院");
    EXPECT_EQ(result[0].word.data(), result.source().data());
    EXPECT_EQ(result[0].position.source, (SourceRange{0, 15}));
}
