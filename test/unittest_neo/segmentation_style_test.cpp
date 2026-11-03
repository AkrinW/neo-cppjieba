#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"

#include "test_paths.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace neo_cppjieba;

namespace {

constexpr auto style = compile_config::segmentation_style;
constexpr auto modes =
    std::array{CutMode::MIX,           CutMode::MP, CutMode::MIX_NO_HMM, CutMode::FULL, CutMode::SEARCH,
               CutMode::SEARCH_NO_HMM, CutMode::HMM};

// Share immutable dictionary/model storage while each call owns its own workspace.
auto style_jieba() -> const Jieba & {
    static const auto jieba = Jieba{DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8", ""};
    return jieba;
}

// Own an isolated dictionary or model fixture until its consuming engine has been destroyed.
class StyleTestFile {
public:
    explicit StyleTestFile(std::string_view contents)
        : directory_(create_temp_directory("jieba-style-")), path_(path_to_utf8(directory_ / "fixture.txt")) {
        auto stream = std::ofstream{directory_ / "fixture.txt", std::ios::binary};
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream << contents;
    }

    ~StyleTestFile() {
        auto error = std::error_code{};
        std::filesystem::remove_all(directory_, error);
    }

    StyleTestFile(const StyleTestFile &) = delete;
    auto operator=(const StyleTestFile &) -> StyleTestFile & = delete;

    [[nodiscard]]
    auto path() const -> const std::string & {
        return path_;
    }

private:
    std::filesystem::path directory_;
    std::string path_;
};

} // namespace

TEST(SegmentationStyleTest, EmptyInputProducesNoTokensInEveryMode) {
    for (const auto mode : modes) {
        EXPECT_TRUE(style_jieba().cut_strings(std::string_view{}, mode).empty());
    }
}

TEST(SegmentationStyleTest, MpUsesTheSelectedAsciiJoiningRule) {
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"I", "B", "M", "1", "2", "3"}
                                                          : std::vector<std::string>{"IBM123"};
    for (const auto mode : {CutMode::MP, CutMode::MIX_NO_HMM, CutMode::SEARCH_NO_HMM}) {
        EXPECT_EQ(style_jieba().cut_strings("IBM123", mode), expected);
    }
}

TEST(SegmentationStyleTest, MixPreservesTheSelectedKnownWordGuard) {
    const auto expected =
        style == SegmentationStyle::CPP ? std::vector<std::string>{"这条"} : std::vector<std::string>{"这", "条"};
    EXPECT_EQ(style_jieba().cut_strings("这条", CutMode::MIX), expected);
    EXPECT_EQ(style_jieba().cut_strings("这条", CutMode::SEARCH), expected);
}

TEST(SegmentationStyleTest, BookTitleMarksUseTheSelectedHmmBoundary) {
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"《", "写", "得", "成", "》"}
                                                          : std::vector<std::string>{"《", "写得成", "》"};
    EXPECT_EQ(style_jieba().cut_strings("《写得成》", CutMode::MIX), expected);
    EXPECT_EQ(style_jieba().cut_strings("《写得成》", CutMode::HMM), expected);
}

TEST(SegmentationStyleTest, SearchExpandsWordsAfterApplyingTheSelectedHmmBoundary) {
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"《", "写", "得", "成", "》"}
                                                          : std::vector<std::string>{"《", "得成", "写得成", "》"};
    EXPECT_EQ(style_jieba().cut_strings("《写得成》", CutMode::SEARCH), expected);
}

TEST(SegmentationStyleTest, SearchPreservesMembershipAcrossUnicodeBlocks) {
    const auto dict = StyleTestFile{"甲乙丙丁 100 n\n甲乙 1 n\n乙丙 1 n\n乙丙丁 1 n\n天地玄黄 100 n\n"};
    const auto jieba = Jieba{dict.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected = std::vector<std::string>{"🙂", " ", "甲乙", "乙丙", "乙丙丁", "甲乙丙丁", " ", "天地玄黄"};
    for (const auto mode : {CutMode::SEARCH, CutMode::SEARCH_NO_HMM}) {
        EXPECT_EQ(jieba.cut_strings("🙂 甲乙丙丁 天地玄黄", mode), expected);
    }
}

TEST(SegmentationStyleTest, SearchClearsMembershipWhenReusingWorkspace) {
    const auto dict = StyleTestFile{"甲乙丙丁 100 n\n甲乙 1 n\n乙丙 1 n\n乙丙丁 1 n\n天地玄黄 100 n\n"};
    const auto jieba = Jieba{dict.path(), DICT_DIR "/hmm_model.utf8", ""};
    auto workspace = Workspace{};
    auto words = std::vector<std::string>{};
    for (const auto mode : {CutMode::SEARCH, CutMode::SEARCH_NO_HMM}) {
        jieba.cut_into("甲乙丙丁", mode, words, workspace);
        EXPECT_EQ(words, (std::vector<std::string>{"甲乙", "乙丙", "乙丙丁", "甲乙丙丁"}));
        jieba.cut_into("天地玄黄", mode, words, workspace);
        EXPECT_EQ(words, (std::vector<std::string>{"天地玄黄"}));
    }
}

TEST(SegmentationStyleTest, MixUsesTheSelectedAlphanumericConnectorRule) {
    auto expected = std::vector<std::string>{"abcxyz", "-", "qwerty", "_", "123.45", "%"};
    if constexpr (style == SegmentationStyle::RUST) {
        expected = {"abcxyz-qwerty_123.45%"};
    } else if constexpr (style == SegmentationStyle::PYTHON) {
        expected = {"abcxyz", "-", "qwerty", "_", "123.45%"};
    }
    EXPECT_EQ(style_jieba().cut_strings("abcxyz-qwerty_123.45%", CutMode::MIX), expected);
}

TEST(SegmentationStyleTest, RustSearchOffersAlphabeticCompoundPartsBeforeTheWholeWord) {
    auto expected = std::vector<std::string>{"abcxyz", "-", "qwerty", "_", "123.45", "%"};
    if constexpr (style == SegmentationStyle::RUST) {
        expected = {"abcxyz", "qwerty", "abcxyz-qwerty_123.45%"};
    } else if constexpr (style == SegmentationStyle::PYTHON) {
        expected = {"abcxyz", "-", "qwerty", "_", "123.45%"};
    }
    EXPECT_EQ(style_jieba().cut_strings("abcxyz-qwerty_123.45%", CutMode::SEARCH), expected);
}

TEST(SegmentationStyleTest, FullUsesTheSelectedCoveredSingletonRule) {
    const auto expected = style == SegmentationStyle::RUST ? std::vector<std::string>{"这", "这条", "条"}
                                                           : std::vector<std::string>{"这条"};
    EXPECT_EQ(style_jieba().cut_strings("这条", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, FullUsesTheSelectedAsciiJoiningRule) {
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"I", "B", "M", "1", "2", "3"}
                                                          : std::vector<std::string>{"IBM123"};
    EXPECT_EQ(style_jieba().cut_strings("IBM123", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, PythonFullRetainsEmptyWhitespaceGaps) {
    const auto expected = style == SegmentationStyle::PYTHON ? std::vector<std::string>{"", " ", "", "\t", "", " ", ""}
                                                             : std::vector<std::string>{" ", "\t", " "};
    EXPECT_EQ(style_jieba().cut_strings(" \t ", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, FullUsesTheSelectedPunctuationGroupingRule) {
    auto expected = std::vector<std::string>{"你好", "！", "！"};
    if constexpr (style == SegmentationStyle::RUST) {
        expected = {"你", "你好", "好", "！", "！"};
    } else if constexpr (style == SegmentationStyle::PYTHON) {
        expected = {"你好", "！！"};
    }
    EXPECT_EQ(style_jieba().cut_strings("你好！！", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, RustFullIncludesLineFeedsInAsciiRuns) {
    auto expected = std::vector<std::string>{"X", "\n", "Y"};
    if constexpr (style == SegmentationStyle::RUST) {
        expected = {"X\nY"};
    } else if constexpr (style == SegmentationStyle::PYTHON) {
        expected = {"X", "", "\n", "", "Y"};
    }
    EXPECT_EQ(style_jieba().cut_strings("X\nY", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, PreciseModesUseTheSelectedCrlfBoundary) {
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"你", "\r", "\n", "好"}
                                                          : std::vector<std::string>{"你", "\r\n", "好"};
    for (const auto mode : {CutMode::MIX, CutMode::MP, CutMode::SEARCH, CutMode::SEARCH_NO_HMM}) {
        EXPECT_EQ(style_jieba().cut_strings("你\r\n好", mode), expected);
    }
}

TEST(SegmentationStyleTest, PreciseModesPreserveSupplementaryRunesAndEmbeddedNul) {
    const auto input = neo_cppjieba::Utf::encode<char>(detail::Unicode{U'😀', U'\0', U'𠀀'});
    const auto expected = std::vector<std::string>{"😀", std::string(1, '\0'), "𠀀"};
    for (const auto mode : {CutMode::MIX, CutMode::MP, CutMode::SEARCH, CutMode::SEARCH_NO_HMM}) {
        EXPECT_EQ(style_jieba().cut_strings(input, mode), expected);
    }
}

TEST(SegmentationStyleTest, FullUsesTheSelectedUnknownCjkRule) {
    const auto dictionary = StyleTestFile{"甲 1 n\n"};
    const auto jieba = Jieba{dictionary.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected =
        style == SegmentationStyle::RUST ? std::vector<std::string>{} : std::vector<std::string>{"乙"};
    EXPECT_EQ(jieba.cut_strings("乙", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, MpUsesTheSelectedTieBreak) {
    const auto dictionary = StyleTestFile{"甲 2 n\n乙 2 n\n丙 2 n\n甲乙 2 n\n乙丙 2 n\n"};
    const auto jieba = Jieba{dictionary.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"甲", "乙丙"}
                                                          : std::vector<std::string>{"甲乙", "丙"};
    EXPECT_EQ(jieba.cut_strings("甲乙丙", CutMode::MP), expected);
}

TEST(SegmentationStyleTest, MpUsesTheSelectedUnknownFrequency) {
    const auto dictionary = StyleTestFile{"甲 10 n\n甲乙 30 n\n乙丙 10 n\n"};
    const auto jieba = Jieba{dictionary.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected = style == SegmentationStyle::CPP ? std::vector<std::string>{"甲乙", "丙"}
                                                          : std::vector<std::string>{"甲", "乙丙"};
    EXPECT_EQ(jieba.cut_strings("甲乙丙", CutMode::MP), expected);
}

TEST(SegmentationStyleTest, DictionaryUnicodeCoverageUsesTheSelectedStyle) {
    const auto dictionary = StyleTestFile{"𠀀𠀁 10 n\n"};
    const auto jieba = Jieba{dictionary.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected =
        style == SegmentationStyle::PYTHON ? std::vector<std::string>{"𠀀", "𠀁"} : std::vector<std::string>{"𠀀𠀁"};
    EXPECT_EQ(jieba.cut_strings("𠀀𠀁", CutMode::MP), expected);
}

TEST(SegmentationStyleTest, HmmUsesTheSelectedLegalTransitionAndTieRules) {
    const auto model = StyleTestFile{"0 0 0 0\n0 0 0 0\n0 0 0 0\n0 0 0 0\n0 0 0 0\n"
                                     "甲:0,乙:0\n甲:0,乙:0\n甲:0,乙:0\n甲:0,乙:0\n"};
    const auto jieba = Jieba{DICT_DIR "/jieba.dict.utf8", model.path(), ""};
    const auto expected =
        style == SegmentationStyle::CPP ? std::vector<std::string>{"甲乙"} : std::vector<std::string>{"甲", "乙"};
    EXPECT_EQ(jieba.cut_strings("甲乙", CutMode::HMM), expected);
}

TEST(SegmentationStyleTest, PythonHmmRecognizesUnicodeDecimalSuffixes) {
    const auto expected = style == SegmentationStyle::PYTHON ? std::vector<std::string>{"12.١٢%"}
                                                             : std::vector<std::string>{"12", ".١٢%"};
    if constexpr (style == SegmentationStyle::CPP) {
        EXPECT_EQ(style_jieba().cut_strings("12.١٢%", CutMode::HMM), (std::vector<std::string>{"12.", "١", "٢", "%"}));
    } else {
        EXPECT_EQ(style_jieba().cut_strings("12.١٢%", CutMode::HMM), expected);
    }
}

TEST(SegmentationStyleTest, PythonFullDoesNotDuplicateOverlappingEnglishText) {
    const auto dictionary = StyleTestFile{"ABCDEFG 5 n\nBCD 3 n\n"};
    const auto jieba = Jieba{dictionary.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected = style == SegmentationStyle::RUST     ? std::vector<std::string>{"ABCDEFG"}
                          : style == SegmentationStyle::PYTHON ? std::vector<std::string>{"BCD", "ABCDEFG"}
                                                               : std::vector<std::string>{"ABCDEFG", "BCD"};
    EXPECT_EQ(jieba.cut_strings("ABCDEFG", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, PythonFullDoesNotJoinEnglishAcrossCoveredGaps) {
    const auto dictionary = StyleTestFile{"BC 5 n\nBCD 3 n\n"};
    const auto jieba = Jieba{dictionary.path(), DICT_DIR "/hmm_model.utf8", ""};
    const auto expected = style == SegmentationStyle::RUST     ? std::vector<std::string>{"ABCDE"}
                          : style == SegmentationStyle::PYTHON ? std::vector<std::string>{"BC", "BCD", "A", "E"}
                                                               : std::vector<std::string>{"A", "BC", "BCD", "E"};
    EXPECT_EQ(jieba.cut_strings("ABCDE", CutMode::FULL), expected);
}

TEST(SegmentationStyleTest, FullEmptyTokensRetainUtf8ByteAndRuneOffsetsAcrossOutputForms) {
    const auto input = std::string{"甲　乙"};
    const auto expected_words = style == SegmentationStyle::PYTHON ? std::vector<std::string>{"甲", "", "　", "", "乙"}
                                                                   : std::vector<std::string>{"甲", "　", "乙"};
    const auto expected_positions =
        style == SegmentationStyle::PYTHON
            ? std::vector<TokenPosition>{{{0, 1}, {0, 3}},
                                         {{1, 1}, {3, 3}},
                                         {{1, 2}, {3, 6}},
                                         {{2, 2}, {6, 6}},
                                         {{2, 3}, {6, 9}}}
            : std::vector<TokenPosition>{{{0, 1}, {0, 3}}, {{1, 2}, {3, 6}}, {{2, 3}, {6, 9}}};
    const auto &jieba = style_jieba();
    auto workspace = Workspace{};
    auto positions = std::vector<TokenPosition>{};
    jieba.cut_into(input, CutMode::FULL, positions, workspace);
    EXPECT_EQ(positions, expected_positions);
    const auto tokens = jieba.cut(input, CutMode::FULL);
    EXPECT_TRUE(std::ranges::equal(tokens.positions(), expected_positions));
    const auto owned = jieba.cut_owned(input, CutMode::FULL);
    EXPECT_TRUE(std::ranges::equal(owned.positions(), expected_positions));
    auto visited = std::vector<std::string>{};
    jieba.cut_each(input, CutMode::FULL, [&](TokenView<char> token) { visited.emplace_back(token.word); }, workspace);
    EXPECT_EQ(visited, expected_words);
    EXPECT_EQ(jieba.cut_strings(input, CutMode::FULL), expected_words);
}

TEST(SegmentationStyleTest, PreciseTokensRetainUtf16AndRuneOffsetsAroundSupplementaryCharacters) {
    const auto input = neo_cppjieba::Utf::encode<char16_t>(detail::Unicode{U'甲', U'😀', U'\0', U'乙'});
    const auto tokens = style_jieba().cut(input, CutMode::MP);
    const auto expected =
        std::vector<TokenPosition>{{{0, 1}, {0, 1}}, {{1, 2}, {1, 3}}, {{2, 3}, {3, 4}}, {{3, 4}, {4, 5}}};
    EXPECT_TRUE(std::ranges::equal(tokens.positions(), expected));
}

TEST(SegmentationStyleTest, RuneAndStringPipelinesUseTheSameStyle) {
    const auto input = std::string_view{"《写得成》 IBM123\r\n甲　乙"};
    const auto runes = Utf::decode(input);
    for (const auto mode : modes) {
        EXPECT_EQ(to_strings(runes, style_jieba().cut_runes(runes, mode)), style_jieba().cut_strings(input, mode));
    }
}

TEST(SegmentationStyleTest, WorkspaceReuseClearsPriorModeResults) {
    auto workspace = Workspace{};
    auto out = std::vector<TokenPosition>{};
    for (const auto mode : modes) {
        style_jieba().cut_into("《写得成》 IBM123", mode, out, workspace);
        EXPECT_FALSE(out.empty());
        style_jieba().cut_into(std::string_view{}, mode, out, workspace);
        EXPECT_TRUE(out.empty());
    }
}
