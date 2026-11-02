#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"
#include "neo/util/Unicode.hpp"

#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace neo_cppjieba;

inline constexpr auto DICT_FILE = std::string_view{DICT_DIR "/jieba.dict.utf8"};
inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};

namespace {

constexpr auto all_modes = std::array{CutMode::MIX,  CutMode::MIX_NO_HMM, CutMode::MP,           CutMode::HMM,
                                      CutMode::FULL, CutMode::SEARCH,     CutMode::SEARCH_NO_HMM};

// Share immutable models between tests of output and lifetime contracts.
auto test_jieba() -> const Jieba & {
    static const auto jieba = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    return jieba;
}

// Consume public token iteration exactly as a caller that needs independent words would.
template <typename Result>
auto copy_words(const Result &tokens) -> std::vector<std::basic_string<typename Result::char_type>> {
    auto words = std::vector<std::basic_string<typename Result::char_type>>{};
    words.reserve(tokens.size());
    for (const auto token : tokens) {
        words.emplace_back(token.word);
    }
    return words;
}

// Use the checked public encoder as an independent reference for source-coordinate conversion.
template <CharType CharT>
auto encode_ranges(std::basic_string_view<CharT> source, std::span<const SourceOffset> offsets,
                   std::span<const WordRange> ranges) -> std::vector<std::basic_string<CharT>> {
    auto words = std::vector<std::basic_string<CharT>>{};
    words.reserve(ranges.size());
    for (const auto range : ranges) {
        words.push_back(neo_cppjieba::detail::encode(source, offsets, range));
    }
    return words;
}

// Materialize explicit rune ranges without relying on the Jieba output adapters.
auto encode_rune_ranges(std::span<const Rune> runes, std::span<const WordRange> ranges) -> std::vector<std::string> {
    auto words = std::vector<std::string>{};
    words.reserve(ranges.size());
    for (const auto range : ranges) {
        words.push_back(neo_cppjieba::Utf::encode<char>(range.slice(runes)));
    }
    return words;
}

// An adapter can return different views; decoding and output copying must use the same conversion.
struct ChangingTextView {
    using value_type = char;
    mutable std::size_t conversions = 0;

    operator std::string_view() const {
        return ++conversions == 1 ? "中国" : "x";
    }
};

// Source-copy optimizations must preserve code-unit offsets for every supported character encoding.
template <typename CharT>
class JiebaCharacterTest : public ::testing::Test {};

using InputCharacterTypes = ::testing::Types<char, char8_t, char16_t, char32_t, wchar_t>;
TYPED_TEST_SUITE(JiebaCharacterTest, InputCharacterTypes);

// Rune spans share the range-returning API across constness and static or dynamic extents.
template <typename Span>
class JiebaRuneSpanTest : public ::testing::Test {};

using RuneSpanTypes =
    ::testing::Types<std::span<Rune>, std::span<const Rune>, std::span<Rune, 5>, std::span<const Rune, 5>>;
TYPED_TEST_SUITE(JiebaRuneSpanTest, RuneSpanTypes);

} // namespace

TYPED_TEST(JiebaCharacterTest, TextInterfacesRejectRawCharacterPointers) {
    using Input = const TypeParam *;
    static_assert(!requires(const Jieba &jieba, Input input) { jieba.cut(input, CutMode::MP); });
    static_assert(!requires(const Jieba &jieba, Input input, Workspace &workspace) {
        jieba.cut_with_workspace(input, CutMode::MP, workspace);
    });
    static_assert(!requires(const Jieba &jieba, Input input, std::vector<TokenPosition> &out, Workspace &workspace) {
        jieba.cut_into(input, CutMode::MP, out, workspace);
    });
    static_assert(!requires(const Jieba &jieba, Input input, Workspace &workspace, void (*emit)(TokenView<TypeParam>)) {
        jieba.cut_each(input, CutMode::MP, emit, workspace);
    });
    static_assert(!requires(const Jieba &jieba, Input input) { jieba.cut_owned(input, CutMode::MP); });
    static_assert(!requires(const Jieba &jieba, Input input) { jieba.cut_strings(input, CutMode::MP); });
}

TEST(JiebaNeoTest, LiteralInputPreservesEmbeddedNulAndOffsets) {
    const auto tokens = test_jieba().cut("😀\0𠀀", CutMode::MP);
    const auto expected_words = std::vector<std::string>{"😀", std::string(1, '\0'), "𠀀"};
    const auto expected_positions = std::vector<TokenPosition>{{{0, 1}, {0, 4}}, {{1, 2}, {4, 5}}, {{2, 3}, {5, 9}}};
    EXPECT_EQ(copy_words(tokens), expected_words);
    EXPECT_TRUE(std::ranges::equal(tokens.positions(), expected_positions));
}

TEST(JiebaNeoTest, ExplicitlySizedPointerInputPreservesEmbeddedNul) {
    const char *input = "😀\0𠀀";
    const auto expected = std::vector<std::string>{"😀", std::string(1, '\0'), "𠀀"};
    EXPECT_EQ(test_jieba().cut_strings(std::string_view{input, 9}, CutMode::MP), expected);
    EXPECT_EQ(test_jieba().cut_strings(std::span{input, std::size_t{9}}, CutMode::MP), expected);
}

TYPED_TEST(JiebaCharacterTest, OutputFormsPreserveWordsAndBothCoordinatesInEveryMode) {
    const auto &jieba = test_jieba();
    const auto inputs = std::array{detail::Unicode{}, detail::Unicode{U'中', U'国', U'科', U'学', U'院'},
                                   detail::Unicode{U'甲', U' ', U'A', U'\0', U'中', U'国', U'😀', U'𠀀', U'乙', U'!'}};
    auto buffer = Workspace{};
    auto source_ranges = std::vector<SourceRange>{};
    auto positions = std::vector<TokenPosition>{};
    auto strings = std::vector<std::basic_string<TypeParam>>{};
    for (const auto &runes : inputs) {
        const auto text = neo_cppjieba::Utf::encode<TypeParam>(runes);
        const auto source = detail::as_view(text);
        const auto decoded = detail::decode_with_offset(source);
        for (const auto mode : all_modes) {
            const auto ranges = jieba.cut_runes(decoded.runes, mode);
            const auto expected = encode_ranges(source, decoded.offsets, ranges);
            const auto tokens = jieba.cut(source, mode);
            const auto reused = jieba.cut_with_workspace(source, mode, buffer);
            const auto owned = jieba.cut_owned(text, mode);
            EXPECT_EQ(copy_words(tokens), expected);
            EXPECT_EQ(copy_words(reused), expected);
            EXPECT_TRUE(std::ranges::equal(reused.positions(), tokens.positions()));
            EXPECT_EQ(copy_words(owned), expected);
            EXPECT_EQ(jieba.cut_strings(source, mode), expected);
            jieba.cut_into(source, mode, source_ranges, buffer);
            jieba.cut_into(source, mode, positions, buffer);
            jieba.cut_into(source, mode, strings, buffer);
            EXPECT_EQ(strings, expected);
            ASSERT_EQ(tokens.size(), ranges.size());
            ASSERT_EQ(reused.size(), ranges.size());
            ASSERT_EQ(source_ranges.size(), ranges.size());
            ASSERT_EQ(positions.size(), ranges.size());
            for (auto i = size_t{0}; i < ranges.size(); ++i) {
                const auto expected_source =
                    SourceRange{decoded.offsets[ranges[i].begin], decoded.offsets[ranges[i].end]};
                EXPECT_EQ(tokens[i].position, (TokenPosition{ranges[i], expected_source}));
                EXPECT_EQ(positions[i], tokens[i].position);
                EXPECT_EQ(source_ranges[i], expected_source);
                EXPECT_EQ(tokens[i].word.data(), source.data() + expected_source.begin);
                EXPECT_EQ(reused[i].word.data(), source.data() + expected_source.begin);
                EXPECT_EQ(owned[i].word.data(), owned.source().data() + expected_source.begin);
            }
            auto visited = size_t{0};
            jieba.cut_each(
                source, mode,
                [&](TokenView<TypeParam> token) {
                    ASSERT_LT(visited, tokens.size());
                    EXPECT_EQ(token.position, tokens[visited].position);
                    EXPECT_EQ(token.word, tokens[visited].word);
                    ++visited;
                },
                buffer);
            EXPECT_EQ(visited, tokens.size());
        }
    }
}

TEST(JiebaNeoTest, BorrowedTokensSurviveWorkspaceReuseReleaseAndDestruction) {
    const auto source = std::string{"中国科学院"};
    const auto tokens = [&] {
        auto workspace = Workspace{};
        auto first = test_jieba().cut_with_workspace(source, CutMode::SEARCH, workspace);
        const auto second = test_jieba().cut_with_workspace("北京", CutMode::MIX, workspace);
        EXPECT_EQ(copy_words(second), (std::vector<std::string>{"北京"}));
        workspace.release();
        const auto third = test_jieba().cut_with_workspace("中国", CutMode::HMM, workspace);
        EXPECT_EQ(copy_words(third), (std::vector<std::string>{"中国"}));
        return first;
    }();
    const auto expected = std::vector<TokenPosition>{
        {{0, 2}, {0, 6}}, {{2, 4}, {6, 12}}, {{3, 5}, {9, 15}}, {{2, 5}, {6, 15}}, {{0, 5}, {0, 15}}};
    EXPECT_TRUE(std::ranges::equal(tokens.positions(), expected));
    EXPECT_EQ(copy_words(tokens), (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
    for (const auto token : tokens) {
        EXPECT_EQ(token.word.data(), source.data() + token.position.source.begin);
    }
}

TEST(JiebaNeoTest, WorkspaceCutConvertsAnAdaptedViewOnce) {
    auto workspace = Workspace{};
    const auto input = ChangingTextView{};
    const auto tokens = test_jieba().cut_with_workspace(input, CutMode::MIX, workspace);
    EXPECT_EQ(copy_words(tokens), (std::vector<std::string>{"中国"}));
    EXPECT_EQ(input.conversions, 1U);
}

TEST(JiebaNeoTest, WorkspaceCutRejectsInvalidTextAndRecovers) {
    auto workspace = Workspace{};
    const auto first = test_jieba().cut_with_workspace("中国科学院", CutMode::SEARCH, workspace);
    EXPECT_THROW(
        static_cast<void>(test_jieba().cut_with_workspace(std::string_view{"中国\xE4\xB8"}, CutMode::MIX, workspace)),
        LogConfig::Exception);
    const auto recovered = test_jieba().cut_with_workspace("北京", CutMode::HMM, workspace);
    EXPECT_EQ(copy_words(recovered), (std::vector<std::string>{"北京"}));
    EXPECT_EQ(copy_words(first), (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
}

TEST(JiebaNeoTest, DirectCutUsesTheSameViewForDecodingAndCopying) {
    const auto input = ChangingTextView{};
    const auto tokens = test_jieba().cut(input, CutMode::MIX);
    EXPECT_EQ(copy_words(tokens), (std::vector<std::string>{"中国"}));
    EXPECT_EQ(input.conversions, 1U);
}

TEST(JiebaNeoTest, PublicSourceEncodingStillRejectsInvalidOffsets) {
    const auto source = std::string_view{"abc"};
    const auto offsets = std::array<SourceOffset, 2>{0, 4};
    EXPECT_THROW(static_cast<void>(neo_cppjieba::detail::encode(source, offsets, WordRange{0, 1})),
                 LogConfig::Exception);
}

TEST(JiebaNeoTest, DirectStringCutReturnsInputEncoding) {
    const auto words = test_jieba().cut_strings(std::string_view{"他来到了网易杭研大厦"}, CutMode::MIX);
    EXPECT_EQ(words, (std::vector<std::string>{"他", "来到", "了", "网易", "杭研", "大厦"}));
}

TEST(JiebaNeoTest, DirectUtf16CutReturnsUtf16Words) {
    const auto words = test_jieba().cut_strings(std::u16string_view{u"我来自北京邮电大学"}, CutMode::MIX);
    EXPECT_EQ(words, (std::vector<std::u16string>{u"我", u"来自", u"北京邮电大学"}));
}

TEST(JiebaNeoTest, ByteInputsReturnUtf8Words) {
    const auto &jieba = test_jieba();
    const auto input = std::u8string_view{u8"他来到了网易杭研大厦"};
    const auto bytes = std::as_bytes(std::span{input.data(), input.size()});
    const auto unsigned_bytes = std::vector<unsigned char>(input.begin(), input.end());
    const auto signed_bytes = std::vector<signed char>(input.begin(), input.end());
    const auto expected = std::vector<std::string>{"他", "来到", "了", "网易", "杭研", "大厦"};
    EXPECT_EQ(jieba.cut_strings(bytes, CutMode::MIX), expected);
    EXPECT_EQ(jieba.cut_strings(unsigned_bytes, CutMode::MIX), expected);
    EXPECT_EQ(jieba.cut_strings(signed_bytes, CutMode::MIX), expected);
    EXPECT_EQ(copy_words(jieba.cut(bytes, CutMode::MIX)), expected);
}

TEST(JiebaNeoTest, ByteOffsetPipelinePreservesOriginalUtf8Words) {
    const auto input = std::u8string_view{u8"中国科学院"};
    const auto bytes = std::as_bytes(std::span{input.data(), input.size()});
    const auto decoded = detail::decode_with_offset(bytes);
    const auto ranges = test_jieba().cut_runes(decoded.runes, CutMode::SEARCH);
    EXPECT_EQ(encode_ranges(detail::as_view(bytes), decoded.offsets, ranges),
              (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
}

TEST(JiebaNeoTest, EmptyByteInputProducesNoWords) {
    EXPECT_TRUE(test_jieba().cut(std::span<const std::byte>{}, CutMode::MIX).empty());
}

TEST(JiebaNeoTest, InvalidByteInputThrowsTheConfiguredException) {
    const auto input = std::array{std::byte{0xE4}, std::byte{0xB8}};
    EXPECT_THROW(static_cast<void>(test_jieba().cut(input, CutMode::MIX)), LogConfig::Exception);
}

TEST(JiebaNeoTest, DirectRuneCutReturnsLocalWordRanges) {
    const auto runes = Utf::decode("甲中国科学院乙");
    const auto input = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    const auto ranges = test_jieba().cut_runes(input, CutMode::SEARCH);
    EXPECT_EQ(encode_rune_ranges(input, ranges),
              (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
}

TYPED_TEST(JiebaRuneSpanTest, SearchReturnsLocalRangesForEveryRuneSpanType) {
    auto runes = Utf::decode("甲中国科学院乙");
    const auto input = TypeParam{runes.data() + 1, size_t{5}};
    const auto ranges = test_jieba().cut_runes(input, CutMode::SEARCH);
    EXPECT_EQ(ranges, (std::vector<WordRange>{{0, 2}, {2, 4}, {3, 5}, {2, 5}, {0, 5}}));
    EXPECT_EQ(encode_rune_ranges(input, ranges),
              (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
}

TYPED_TEST(JiebaRuneSpanTest, PartitionModesAgreeForEveryRuneSpanType) {
    const auto &jieba = test_jieba();
    auto runes = Utf::decode("中国科学院");
    const auto input = TypeParam{runes.data(), size_t{5}};
    const auto whole_word = std::vector<WordRange>{{0, 5}};
    EXPECT_EQ(jieba.cut_runes(input, CutMode::MIX), whole_word);
    EXPECT_EQ(jieba.cut_runes(input, CutMode::MIX_NO_HMM), whole_word);
    EXPECT_EQ(jieba.cut_runes(input, CutMode::MP), whole_word);
    EXPECT_EQ(jieba.cut_runes(input, CutMode::HMM), (std::vector<WordRange>{{0, 2}, {2, 5}}));
}

TEST(JiebaNeoTest, EmptyFixedRuneSpansProduceNoWordsInEveryMode) {
    for (const auto mode : all_modes) {
        EXPECT_TRUE(test_jieba().cut_runes(std::span<Rune, 0>{}, mode).empty());
        EXPECT_TRUE(test_jieba().cut_runes(std::span<const Rune, 0>{}, mode).empty());
    }
}

TEST(JiebaNeoTest, ManualUnicodePipelineReturnsWordRanges) {
    const auto runes = Utf::decode("我来自北京邮电大学");
    const auto ranges = test_jieba().cut_runes(runes, CutMode::FULL);
    EXPECT_EQ(encode_rune_ranges(runes, ranges),
              (std::vector<std::string>{"我", "来自", "北京", "北京邮电", "北京邮电大学", "邮电", "邮电大学", "电大",
                                        "大学"}));
}

TEST(JiebaNeoTest, ManualOffsetPipelineMatchesDirectSearchCut) {
    const auto sentence = std::string_view{"中国科学院，后在日本京都大学深造"};
    const auto decoded = detail::decode_with_offset(sentence);
    const auto ranges = test_jieba().cut_runes(decoded.runes, CutMode::SEARCH);
    EXPECT_EQ(encode_ranges(sentence, decoded.offsets, ranges), test_jieba().cut_strings(sentence, CutMode::SEARCH));
}

TEST(JiebaNeoTest, ExplicitNoHmmModePreservesUnknownWordBoundaries) {
    const auto words = test_jieba().cut_strings("他来到了网易杭研大厦", CutMode::MIX_NO_HMM);
    EXPECT_EQ(join(words), "他/来到/了/网易/杭/研/大厦");
}

TEST(JiebaNeoTest, ExplicitCutModesPreserveExpectedWords) {
    const auto &jieba = test_jieba();
    const auto sentence = std::string_view{"中国科学院"};
    EXPECT_EQ(copy_words(jieba.cut(sentence, CutMode::MIX)), (std::vector<std::string>{"中国科学院"}));
    EXPECT_EQ(copy_words(jieba.cut(sentence, CutMode::SEARCH)),
              (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
    EXPECT_EQ(copy_words(jieba.cut(sentence, CutMode::MP)), (std::vector<std::string>{"中国科学院"}));
    EXPECT_EQ(copy_words(jieba.cut(sentence, CutMode::HMM)), (std::vector<std::string>{"中国", "科学院"}));
}

TEST(JiebaNeoTest, BorrowedSubviewKeepsRelativeByteOffsetsAndOriginalStorage) {
    const auto text = std::string{"前中国科学院后"};
    const auto source = std::string_view{text}.substr(3, 15);
    const auto tokens = test_jieba().cut(source, CutMode::SEARCH);
    const auto expected = std::vector<TokenPosition>{
        {{0, 2}, {0, 6}}, {{2, 4}, {6, 12}}, {{3, 5}, {9, 15}}, {{2, 5}, {6, 15}}, {{0, 5}, {0, 15}}};
    ASSERT_EQ(tokens.size(), expected.size());
    EXPECT_TRUE(std::ranges::equal(tokens.positions(), expected));
    for (const auto token : tokens) {
        EXPECT_EQ(token.word.data(), source.data() + token.position.source.begin);
    }
}

TEST(JiebaNeoTest, CutIntoReplacesOutputAndRetainsReservedCapacity) {
    auto buffer = Workspace{};
    auto out = std::vector<SourceRange>{{99, 100}};
    out.reserve(64);
    const auto capacity = out.capacity();
    test_jieba().cut_into("中国科学院", CutMode::SEARCH, out, buffer);
    EXPECT_EQ(out, (std::vector<SourceRange>{{0, 6}, {6, 12}, {9, 15}, {6, 15}, {0, 15}}));
    EXPECT_EQ(out.capacity(), capacity);
    test_jieba().cut_into("", CutMode::MIX, out, buffer);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(out.capacity(), capacity);
}

TEST(JiebaNeoTest, SeparateResultsSurviveWorkspaceReuseAndRelease) {
    auto buffer = Workspace{};
    auto first = std::vector<TokenPosition>{};
    auto second = std::vector<TokenPosition>{};
    test_jieba().cut_into("中国科学院", CutMode::SEARCH, first, buffer);
    const auto expected = first;
    test_jieba().cut_into("北京", CutMode::MIX, second, buffer);
    buffer.release();
    EXPECT_EQ(first, expected);
    EXPECT_EQ(second, (std::vector<TokenPosition>{{{0, 2}, {0, 6}}}));
    test_jieba().cut_into("中国", CutMode::MIX, second, buffer);
    EXPECT_EQ(second, (std::vector<TokenPosition>{{{0, 2}, {0, 6}}}));
}

TEST(JiebaNeoTest, RuneOutputCanBeReusedWithoutChangingCoordinates) {
    auto buffer = Workspace{};
    const auto runes = Utf::decode("中国科学院");
    auto out = std::vector<WordRange>{{99, 100}};
    out.reserve(32);
    const auto capacity = out.capacity();
    test_jieba().cut_runes_into(runes, CutMode::SEARCH, out, buffer);
    EXPECT_EQ(out, (std::vector<WordRange>{{0, 2}, {2, 4}, {3, 5}, {2, 5}, {0, 5}}));
    test_jieba().cut_runes_into(runes, CutMode::MP, out, buffer);
    EXPECT_EQ(out, (std::vector<WordRange>{{0, 5}}));
    EXPECT_EQ(out.capacity(), capacity);
}

TEST(JiebaNeoTest, RuneOutputAndWorkspaceCanBeReusedAfterInvalidMode) {
    auto workspace = Workspace{};
    const auto runes = Utf::decode("中国科学院");
    auto out = std::vector<WordRange>{{99, 100}};
    const auto invalid_mode = static_cast<CutMode>(uint8_t{255});

    EXPECT_THROW(test_jieba().cut_runes_into(runes, invalid_mode, out, workspace), LogConfig::Exception);
    EXPECT_TRUE(out.empty());
    test_jieba().cut_runes_into(runes, CutMode::MP, out, workspace);
    EXPECT_EQ(out, (std::vector<WordRange>{{0, 5}}));
}

TEST(JiebaNeoTest, OwnedSmallTextSurvivesResultMove) {
    auto original = test_jieba().cut_owned(std::string{"中国"}, CutMode::MIX);
    const auto moved = std::move(original);
    EXPECT_EQ(moved.source(), "中国");
    ASSERT_EQ(moved.size(), 1U);
    EXPECT_EQ(moved[0].word, "中国");
    EXPECT_EQ(moved[0].word.data(), moved.source().data());
}

TEST(JiebaNeoTest, OwnedLongTextSurvivesSourceChangesCopyAndMove) {
    auto input = std::string{"他来到了网易杭研大厦，中国科学院，𠮷😀"};
    const auto expected = input;
    auto original = test_jieba().cut_owned(input, CutMode::SEARCH);
    const auto copied = original;
    const auto moved = std::move(original);
    input.assign("changed");
    EXPECT_EQ(copied.source(), expected);
    EXPECT_EQ(moved.source(), expected);
    EXPECT_EQ(copy_words(copied), copy_words(moved));
    for (const auto token : copied) {
        EXPECT_EQ(token.word.data(), copied.source().data() + token.position.source.begin);
    }
    for (const auto token : moved) {
        EXPECT_EQ(token.word.data(), moved.source().data() + token.position.source.begin);
    }
}

TEST(JiebaNeoTest, VisitorExceptionPropagatesAndWorkspaceRemainsReusable) {
    auto buffer = Workspace{};
    const auto fail = [](TokenView<char>) -> void {
        throw std::runtime_error("visitor failure");
    };
    EXPECT_THROW(test_jieba().cut_each("中国科学院", CutMode::SEARCH, fail, buffer), std::runtime_error);
    auto words = std::vector<std::string>{};
    test_jieba().cut_each("中国", CutMode::MIX, [&](TokenView<char> token) { words.emplace_back(token.word); }, buffer);
    EXPECT_EQ(words, (std::vector<std::string>{"中国"}));
}

TEST(JiebaNeoTest, VisitorCanUseAnotherWorkspaceWithoutChangingOuterResults) {
    auto outer = Workspace{};
    auto inner = Workspace{};
    auto words = std::vector<std::string>{};
    test_jieba().cut_each(
        "中国科学院", CutMode::SEARCH,
        [&](TokenView<char> token) {
            auto nested = std::vector<SourceRange>{};
            test_jieba().cut_into("北京", CutMode::MIX, nested, inner);
            EXPECT_EQ(nested, (std::vector<SourceRange>{{0, 6}}));
            words.emplace_back(token.word);
        },
        outer);
    EXPECT_EQ(words, (std::vector<std::string>{"中国", "科学", "学院", "科学院", "中国科学院"}));
}

TEST(JiebaNeoTest, InvalidTextDoesNotInvokeVisitorAndWorkspaceCanRecover) {
    auto buffer = Workspace{};
    auto calls = size_t{0};
    const auto count = [&](TokenView<char>) {
        ++calls;
    };
    EXPECT_THROW(test_jieba().cut_each(std::string_view{"中国\xE4\xB8"}, CutMode::MIX, count, buffer),
                 LogConfig::Exception);
    EXPECT_EQ(calls, 0U);
    test_jieba().cut_each("北京", CutMode::MIX, count, buffer);
    EXPECT_EQ(calls, 1U);
}

TEST(JiebaNeoTest, TextEntriesRejectMalformedUtfInEveryMode) {
    const auto utf8 = std::string_view{"\xD6\xD0\xB9\xFA"};
    const auto utf16 = std::u16string{u'中', char16_t{0xD800}};
    const auto utf32 = std::u32string{U'中', char32_t{0x110000}};
    for (const auto mode : all_modes) {
        EXPECT_THROW(static_cast<void>(test_jieba().cut(utf8, mode)), LogConfig::Exception);
        EXPECT_THROW(static_cast<void>(test_jieba().cut(utf16, mode)), LogConfig::Exception);
        EXPECT_THROW(static_cast<void>(test_jieba().cut(utf32, mode)), LogConfig::Exception);
    }
}

TEST(JiebaNeoTest, TextEntryChecksCurrentBytesAfterSuccessfulPreflight) {
    auto input = std::string{"中国"};
    ASSERT_TRUE(Utf::is_valid(input));
    input.back() = char{0x7F};
    EXPECT_THROW(static_cast<void>(test_jieba().cut(input, CutMode::MIX)), LogConfig::Exception);
}

TEST(JiebaNeoTest, UnknownModesUseTheConfiguredErrorPathForEveryEntryPoint) {
    const auto &jieba = test_jieba();
    const auto mode = static_cast<CutMode>(uint8_t{255});
    auto buffer = Workspace{};
    auto source_ranges = std::vector<SourceRange>{};
    auto rune_ranges = std::vector<WordRange>{};
    const auto runes = Utf::decode("中国");
    EXPECT_THROW(static_cast<void>(jieba.cut("中国", mode)), LogConfig::Exception);
    EXPECT_THROW(static_cast<void>(jieba.cut_with_workspace("中国", mode, buffer)), LogConfig::Exception);
    const auto recovered = jieba.cut_with_workspace("北京", CutMode::MIX, buffer);
    EXPECT_EQ(copy_words(recovered), (std::vector<std::string>{"北京"}));
    EXPECT_THROW(static_cast<void>(jieba.cut_strings("中国", mode)), LogConfig::Exception);
    EXPECT_THROW(static_cast<void>(jieba.cut_owned(std::string{"中国"}, mode)), LogConfig::Exception);
    EXPECT_THROW(jieba.cut_into("中国", mode, source_ranges, buffer), LogConfig::Exception);
    EXPECT_THROW(jieba.cut_each("中国", mode, [](TokenView<char>) {}, buffer), LogConfig::Exception);
    EXPECT_THROW(static_cast<void>(jieba.cut_runes(runes, mode)), LogConfig::Exception);
    EXPECT_THROW(jieba.cut_runes_into(runes, mode, rune_ranges, buffer), LogConfig::Exception);
}

TEST(JiebaNeoTest, WorkspaceMatchesFreshResultsAfterGrowthAndModeChanges) {
    const auto &jieba = test_jieba();
    auto workspace = Workspace{};
    auto positions = std::vector<TokenPosition>{};
    auto long_text = std::string{};
    constexpr auto fragment = std::string_view{"小明来到中国科学院，研究量子计算ABC123；𠀀😀！"};
    long_text.reserve(fragment.size() * 64);
    for (auto i = 0; i < 64; ++i) {
        long_text.append(fragment);
    }
    const auto inputs = std::array<std::string_view, 6>{long_text, "中国科学院", "", "， !", "𠀀😀AB 3.14", long_text};
    for (const auto input : inputs) {
        for (const auto mode : all_modes) {
            const auto expected = jieba.cut(input, mode);
            const auto tokens = jieba.cut_with_workspace(input, mode, workspace);
            EXPECT_EQ(copy_words(tokens), copy_words(expected));
            EXPECT_TRUE(std::ranges::equal(tokens.positions(), expected.positions()));
            jieba.cut_into(input, mode, positions, workspace);
            ASSERT_EQ(positions.size(), expected.size());
            for (auto i = size_t{0}; i < positions.size(); ++i) {
                EXPECT_EQ(positions[i], expected[i].position);
            }
        }
    }
}

TEST(JiebaNeoTest, WorkspaceCanMoveAndBeReleasedWithoutChangingStoredResults) {
    auto workspace = Workspace{};
    auto first = std::vector<TokenPosition>{};
    auto second = std::vector<TokenPosition>{};
    test_jieba().cut_into("中国科学院", CutMode::SEARCH, first, workspace);
    const auto expected = first;
    auto moved = std::move(workspace);
    test_jieba().cut_into("北京", CutMode::MIX, second, moved);
    moved.release();
    EXPECT_EQ(first, expected);
    EXPECT_EQ(second, (std::vector<TokenPosition>{{{0, 2}, {0, 6}}}));
    test_jieba().cut_into("中国", CutMode::MIX, second, workspace);
    EXPECT_EQ(second, (std::vector<TokenPosition>{{{0, 2}, {0, 6}}}));
}

TEST(JiebaNeoTest, InvalidTextClearsOutputAndWorkspaceCanRecoverInAnotherMode) {
    auto workspace = Workspace{};
    auto out = std::vector<TokenPosition>{};
    test_jieba().cut_into("中国科学院", CutMode::SEARCH, out, workspace);
    EXPECT_THROW(test_jieba().cut_into(std::string_view{"中国\xE4\xB8"}, CutMode::MIX, out, workspace),
                 LogConfig::Exception);
    EXPECT_TRUE(out.empty());
    test_jieba().cut_into("北京", CutMode::HMM, out, workspace);
    EXPECT_EQ(out, (std::vector<TokenPosition>{{{0, 2}, {0, 6}}}));
}

TEST(JiebaNeoTest, AddedWordJoinsDictionarySegmentation) {
    auto jieba = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    const auto text = std::string_view{"他来到了网易杭研大厦"};
    ASSERT_EQ(std::ranges::count(jieba.cut_strings(text, CutMode::MP), "杭研"), 0);

    jieba.add_word("杭研", PosTag{"nz"});

    EXPECT_EQ(std::ranges::count(jieba.cut_strings(text, CutMode::MP), "杭研"), 1);
    EXPECT_EQ(jieba.lookup_tag("杭研"), "nz");
}

TEST(JiebaNeoTest, RemovedWordLeavesDictionarySegmentation) {
    auto jieba = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    const auto text = std::string_view{"他来到了网易杭研大厦"};
    ASSERT_EQ(std::ranges::count(jieba.cut_strings(text, CutMode::MP), "大厦"), 1);

    EXPECT_TRUE(jieba.remove_word("大厦"));

    EXPECT_EQ(std::ranges::count(jieba.cut_strings(text, CutMode::MP), "大厦"), 0);
    EXPECT_FALSE(jieba.remove_word("大厦"));
}

TEST(JiebaNeoTest, RuntimeWordsMatchUserDictionaryEntries) {
    const auto user_dicts = std::array{DictSource::memory("杭研 5 nz\n研 n\n", "user.dict")};
    const auto loaded = Jieba{DictSource::file(DICT_FILE), DictSource::file(HMM_MODEL_FILE), user_dicts};
    auto edited = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};

    edited.add_word_with_freq("杭研", 5, PosTag{"nz"});
    edited.add_word("研", PosTag{"n"});

    for (const auto text : {std::string_view{"他来到了网易杭研大厦"}, std::string_view{"研究生命的起源"}}) {
        for (const auto mode : all_modes) {
            EXPECT_EQ(edited.cut_strings<Tagging::Pos>(text, mode), loaded.cut_strings<Tagging::Pos>(text, mode))
                << text << " mode " << static_cast<int>(mode);
        }
    }
}

TEST(JiebaNeoTest, RuntimeEditsRejectInvalidWords) {
    auto jieba = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};

    EXPECT_THROW(jieba.add_word(std::string_view{}, PosTag{"n"}), LogConfig::Exception);
    EXPECT_THROW(jieba.add_word(std::string_view{"\xff"}, PosTag{"n"}), LogConfig::Exception);
    EXPECT_THROW(jieba.add_word_with_freq(std::string_view{"杭研"}, 0, PosTag{"n"}), LogConfig::Exception);
    EXPECT_THROW(jieba.remove_word(std::string_view{}), LogConfig::Exception);
    EXPECT_FALSE(jieba.remove_word("杭研"));
}

TEST(JiebaNeoTest, WholeDictionaryCopiesRequireClone) {
    static_assert(!std::is_copy_constructible_v<Jieba>);
    static_assert(!std::is_copy_assignable_v<Jieba>);
    static_assert(std::is_nothrow_move_constructible_v<Jieba>);
    static_assert(std::is_nothrow_move_assignable_v<Jieba>);
}

TEST(JiebaNeoTest, CloneSegmentsIndependentlyAfterEdits) {
    const auto original = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    auto copy = original.clone();
    const auto text = std::string_view{"他来到了网易杭研大厦"};

    copy.add_word("杭研", PosTag{"nz"});

    EXPECT_EQ(std::ranges::count(original.cut_strings(text, CutMode::MP), "杭研"), 0);
    EXPECT_EQ(std::ranges::count(copy.cut_strings(text, CutMode::MP), "杭研"), 1);
}

TEST(JiebaNeoTest, CompactKeepsSegmentationAfterEdits) {
    auto jieba = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    jieba.add_word("杭研", PosTag{"nz"});
    jieba.add_word_with_freq("研大", 3, PosTag{"n"});
    ASSERT_TRUE(jieba.remove_word("大厦"));
    ASSERT_TRUE(jieba.remove_word("研究生"));
    constexpr auto texts = std::array{std::string_view{"他来到了网易杭研大厦"}, std::string_view{"研究生命的起源"}};
    auto expected = std::vector<std::vector<TaggedWord<char>>>{};
    for (const auto text : texts) {
        for (const auto mode : all_modes) {
            expected.push_back(jieba.cut_strings<Tagging::Pos>(text, mode));
        }
    }

    jieba.compact();

    auto index = std::size_t{0};
    for (const auto text : texts) {
        for (const auto mode : all_modes) {
            EXPECT_EQ(jieba.cut_strings<Tagging::Pos>(text, mode), expected[index++])
                << text << " mode " << static_cast<int>(mode);
        }
    }
}
