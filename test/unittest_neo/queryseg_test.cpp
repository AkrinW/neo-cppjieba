#include "../QuerySegmentCompare.hpp"
#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/QuerySegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include "test_paths.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

inline constexpr auto DICT_FILE = std::string_view{DICT_DIR "/jieba.dict.utf8"};
inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};

namespace {

// Force MP to select single letters while retaining low-frequency sub-words for HMM search tokens.
class QuerySegmentHmmWordsTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = create_temp_directory("neo-query-hmm-");
        write_dictionary("ab 1 n\nbc 1 n\ncd 1 n\nabc 1 n\nbcd 1 n\nabcd 1 n\n");
    }

    void TearDown() override {
        if (!directory_.empty()) {
            auto error = std::error_code{};
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    [[nodiscard]] auto dictionary_path() const -> std::string {
        return path_to_utf8(directory_ / "main.dict");
    }

    void write_dictionary(std::string_view sub_words) const {
        auto output = std::ofstream{directory_ / "main.dict", std::ios::binary};
        ASSERT_TRUE(output.is_open());
        output << "a 100000 n\nb 100000 n\nc 100000 n\nd 100000 n\n" << sub_words;
        output.close();
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(QuerySegmentHmmWordsTest, IncludesDictionarySubwordsOutsideTheSelectedMpPath) {
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode(std::string_view{"abcd"});
    ASSERT_EQ(cut<QuerySegment<false>>(dict, model, runes), (std::vector<WordRange>{{0, 1}, {1, 2}, {2, 3}, {3, 4}}));

    const auto result = cut<QuerySegment<true>>(dict, model, runes);
    EXPECT_EQ(to_strings(runes, result), (std::vector<std::string>{"ab", "bc", "cd", "abc", "bcd", "abcd"}));
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 2}, {1, 3}, {2, 4}, {0, 3}, {1, 4}, {0, 4}}));
    EXPECT_EQ(result, test::query_cut_requery(dict, model, runes));
}

TEST_F(QuerySegmentHmmWordsTest, PreservesSubwordRuneOffsetsAfterSeparators) {
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode(std::string_view{"前𠮷， abcd后"});
    const auto input = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    const auto result = cut<QuerySegment<true>>(dict, model, input);

    EXPECT_EQ(to_strings(input, result),
              (std::vector<std::string>{"𠮷", "，", " ", "ab", "bc", "cd", "abc", "bcd", "abcd"}));
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 1}, {1, 2}, {2, 3}, {3, 5}, {4, 6}, {5, 7}, {3, 6}, {4, 7}, {3, 7}}));
    EXPECT_EQ(result, test::query_cut_requery(dict, model, input));
}

TEST_F(QuerySegmentHmmWordsTest, OmitsMissingSubwordsWhenLongerDictionaryMatchesExist) {
    ASSERT_NO_FATAL_FAILURE(write_dictionary("ab 1 n\nabc 1 n\nabcd 1 n\nbcd 1 n\ncdef 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode(std::string_view{"abcd"});
    const auto result = cut<QuerySegment<true>>(dict, model, runes);

    EXPECT_EQ(to_strings(runes, result), (std::vector<std::string>{"ab", "abc", "bcd", "abcd"}));
    EXPECT_EQ(result, test::query_cut_requery(dict, model, runes));
}

TEST_F(QuerySegmentHmmWordsTest, ReusedScratchPreservesWordsAcrossDifferentInputsAndModes) {
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto long_input = decode("abcd");
    const auto short_input = decode("dcb");
    const auto expanded = std::vector<WordRange>{{0, 2}, {1, 3}, {2, 4}, {0, 3}, {1, 4}, {0, 4}};
    auto scratch = detail::SegmentScratch{};
    auto result = std::vector<WordRange>{};

    QuerySegment<true>::cut_into(dict, model, long_input, result, scratch);
    EXPECT_EQ(result, expanded);
    QuerySegment<true>::cut_into(dict, model, short_input, result, scratch);
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 3}}));
    QuerySegment<true>::cut_into(dict, model, std::span<const Rune>{}, result, scratch);
    EXPECT_TRUE(result.empty());
    QuerySegment<false>::cut_into(dict, model, long_input, result, scratch);
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 1}, {1, 2}, {2, 3}, {3, 4}}));
    QuerySegment<true>::cut_into(dict, model, long_input, result, scratch);
    EXPECT_EQ(result, expanded);
}

TEST_F(QuerySegmentHmmWordsTest, NonBmpSubwordsUseRuneOffsetsAfterASeparator) {
    ASSERT_NO_FATAL_FAILURE(
        write_dictionary("𠮷甲😀乙 100000 n\n𠮷甲 1 n\n甲😀 1 n\n😀乙 1 n\n𠮷甲😀 1 n\n甲😀乙 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode("前，𠮷甲😀乙");
    const auto expected = std::vector<WordRange>{{0, 1}, {1, 2}, {2, 4}, {3, 5}, {4, 6}, {2, 5}, {3, 6}, {2, 6}};

    EXPECT_EQ(cut<QuerySegment<true>>(dict, model, runes), expected);
    EXPECT_EQ(cut<QuerySegment<false>>(dict, model, runes), expected);
}

TEST(QuerySegmentNeoTest, HmmWordsSurroundingDictionaryWordsPreserveOrder) {
    const auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode("杭研中国科学院杭研");
    const auto result = cut<QuerySegment<true>>(dict, model, runes);

    EXPECT_EQ(to_strings(runes, result),
              (std::vector<std::string>{"杭研", "中国", "科学", "学院", "科学院", "中国科学院", "杭研"}));
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 2}, {2, 4}, {4, 6}, {5, 7}, {4, 7}, {2, 7}, {7, 9}}));
}

TEST(QuerySegmentNeoTest, EmptyInput) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = Unicode{};
    auto result = cut<QuerySegment<>>(dict, model, runes);
    EXPECT_TRUE(result.empty());
}

TEST(QuerySegmentNeoTest, SingleChar) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"我"});
    auto result = cut<QuerySegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    ASSERT_EQ(words.size(), 1U);
    EXPECT_EQ(words[0], "我");
}

TEST(QuerySegmentNeoTest, SpanSubrangeUsesLocalOffsets) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"甲中国科学院乙"});
    auto span = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    auto result = cut<QuerySegment<>>(dict, model, span);
    auto words = to_strings(span, result);

    EXPECT_EQ(words, std::vector<std::string>({"中国", "科学", "学院", "科学院", "中国科学院"}));
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result.front().begin, 0u);
    EXPECT_EQ(result.back().end, static_cast<RuneIndex>(span.size()));
}

TEST(QuerySegmentNeoTest, TwoCharWord) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"亲口交代"});
    auto result = cut<QuerySegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    EXPECT_EQ(join(words), "亲口/交代") << "actual: " << join(words);
}

TEST(QuerySegmentNeoTest, ClassicSentence) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"小明硕士毕业于中国科学院计算所，后在日本京都大学深造"});
    auto result = cut<QuerySegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    auto expected = std::string{
        "小明/硕士/毕业/于/中国/科学/学院/科学院/中国科学院/计算/计算所/，/后/在/日本/京都/大学/日本京都大学/深造"};
    EXPECT_EQ(join(words), expected) << "actual: " << join(words);
}

TEST(QuerySegmentNeoTest, SubWordExtraction) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"他心理健康"});
    auto result = cut<QuerySegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    EXPECT_EQ(join(words), "他/心理/健康/心理健康") << "actual: " << join(words);
}

TEST(QuerySegmentNeoTest, ChineseAcademy) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"中国科学院"});
    auto result = cut<QuerySegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    EXPECT_EQ(join(words), "中国/科学/学院/科学院/中国科学院") << "actual: " << join(words);
}

TEST(QuerySegmentNeoTest, UnicodeOverloadWithSeparators) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string_view{"中国科学院，后在日本京都大学深造"};
    auto runes = decode(sentence);
    auto result = cut<QuerySegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    auto actual = join(words);
    EXPECT_NE(actual.find("中国科学院"), std::string::npos) << "actual: " << actual;
    EXPECT_NE(actual.find("，"), std::string::npos) << "actual: " << actual;
    EXPECT_NE(actual.find("京都"), std::string::npos) << "actual: " << actual;
}

TEST(QuerySegmentNeoTest, NoHmm) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"中国科学院"});
    auto result_hmm = cut<QuerySegment<>>(dict, model, runes);
    auto result_no = cut<QuerySegment<false>>(dict, model, runes);

    EXPECT_EQ(to_strings(runes, result_hmm), to_strings(runes, result_no));
}

TEST(QuerySegmentNeoTest, ReuseDagMatchesRequery) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"小明硕士毕业于中国科学院计算所，后在日本京都大学深造"});

    auto result_requery = test::query_cut_requery(dict, model, runes);
    auto result_buffered = test::query_cut_buffered_dag(dict, model, runes);
    auto result_inline = cut<QuerySegment<>>(dict, model, runes);

    EXPECT_EQ(result_buffered, result_requery);
    EXPECT_EQ(result_inline, result_requery);
    EXPECT_EQ(to_strings(runes, result_buffered), to_strings(runes, result_requery));
    EXPECT_EQ(to_strings(runes, result_inline), to_strings(runes, result_requery));
}

TEST(QuerySegmentNeoTest, ReuseDagMatchesRequeryWithoutHmm) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"他来到了网易杭研大厦"});

    auto result_requery = test::query_cut_requery<false>(dict, model, runes);
    auto result_buffered = test::query_cut_buffered_dag<false>(dict, model, runes);
    auto result_inline = cut<QuerySegment<false>>(dict, model, runes);

    EXPECT_EQ(result_buffered, result_requery);
    EXPECT_EQ(result_inline, result_requery);
    EXPECT_EQ(to_strings(runes, result_buffered), to_strings(runes, result_requery));
    EXPECT_EQ(to_strings(runes, result_inline), to_strings(runes, result_requery));
}
