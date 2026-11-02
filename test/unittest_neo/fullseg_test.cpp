#include "../SegmentTestUtils.hpp"
#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/algorithm/FullSegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include "test_paths.h"

#include <algorithm>
#include <cstdlib>
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

namespace {

// Small dictionaries isolate FULL's overlap, coverage and separator rules.
class FullSegmentDictionaryTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = create_temp_directory("neo-full-");
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

    void write_dictionary(std::string_view contents) const {
        auto output = std::ofstream{directory_ / "main.dict", std::ios::binary};
        ASSERT_TRUE(output.is_open());
        output << contents;
        output.close();
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(FullSegmentDictionaryTest, OverlappingWordsKeepStartThenEndOrder) {
    ASSERT_NO_FATAL_FAILURE(write_dictionary("甲 1 n\n甲乙 1 n\n甲乙丙 1 n\n乙丙 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙丙");

    EXPECT_EQ(cut<FullSegment>(dict, unused_model(), runes), (std::vector<WordRange>{{0, 2}, {0, 3}, {1, 3}}));
}

TEST_F(FullSegmentDictionaryTest, ShorterLaterMatchesPreserveEarlierCoverage) {
    ASSERT_NO_FATAL_FAILURE(write_dictionary("甲乙丙丁戊 1 n\n乙丙 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙丙丁戊己");

    EXPECT_EQ(cut<FullSegment>(dict, unused_model(), runes), (std::vector<WordRange>{{0, 5}, {1, 3}, {5, 6}}));
}

TEST_F(FullSegmentDictionaryTest, UnmatchedPrefixesAndUnknownRunesRemainSingleTokens) {
    ASSERT_NO_FATAL_FAILURE(write_dictionary("甲乙 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("𠮷甲丙😀");

    EXPECT_EQ(to_strings(runes, cut<FullSegment>(dict, unused_model(), runes)),
              (std::vector<std::string>{"𠮷", "甲", "丙", "😀"}));
}

TEST_F(FullSegmentDictionaryTest, NonBmpMatchesKeepRuneOffsetsAcrossSeparators) {
    ASSERT_NO_FATAL_FAILURE(write_dictionary("𠮷😀 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode(" \t𠮷😀，𠮷\n😀。");

    EXPECT_EQ(cut<FullSegment>(dict, unused_model(), runes),
              (std::vector<WordRange>{{0, 1}, {1, 2}, {2, 4}, {4, 5}, {5, 6}, {6, 7}, {7, 8}, {8, 9}}));
}

TEST_F(FullSegmentDictionaryTest, ReusedScratchReplacesLongShortAndEmptyResults) {
    ASSERT_NO_FATAL_FAILURE(write_dictionary("甲乙丙丁戊 1 n\n乙丙 1 n\n"));
    const auto dict = DictTrie{dictionary_path(), "", UserWordWeightOption::WordWeightMedian};
    auto scratch = detail::SegmentScratch{};
    auto words = std::vector<WordRange>{};

    FullSegment::cut_into(dict, unused_model(), decode("甲乙丙丁戊己"), words, scratch);
    EXPECT_EQ(words, (std::vector<WordRange>{{0, 5}, {1, 3}, {5, 6}}));
    FullSegment::cut_into(dict, unused_model(), decode("𠮷"), words, scratch);
    EXPECT_EQ(words, (std::vector<WordRange>{{0, 1}}));
    FullSegment::cut_into(dict, unused_model(), Unicode{}, words, scratch);
    EXPECT_TRUE(words.empty());
    FullSegment::cut_into(dict, unused_model(), decode("乙丙"), words, scratch);
    EXPECT_EQ(words, (std::vector<WordRange>{{0, 2}}));
}

TEST(FullSegmentTest, EmptyInput) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = Unicode{};
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    EXPECT_TRUE(result.empty());
}

TEST(FullSegmentTest, SingleCharDictWord) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"我"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);
    ASSERT_EQ(words.size(), 1U);
    EXPECT_EQ(words[0], "我");
}

TEST(FullSegmentTest, ClassicSentence) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"我来自北京邮电大学"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);

    auto expected =
        std::vector<std::string>{"我", "来自", "北京", "北京邮电", "北京邮电大学", "邮电", "邮电大学", "电大", "大学"};
    EXPECT_EQ(words, expected) << "actual: " << join(words);
}

TEST(FullSegmentTest, AnotherSentence) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"他来到了网易杭研大厦"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);

    auto has = [&](const std::string &w) {
        return std::ranges::find(words, w) != words.end();
    };
    EXPECT_TRUE(has("来到")) << "actual: " << join(words);
    EXPECT_TRUE(has("大厦")) << "actual: " << join(words);
    EXPECT_TRUE(has("他")) << "actual: " << join(words);
}

TEST(FullSegmentTest, NanjingBridge) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"南京市长江大桥"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);

    auto has = [&](const std::string &w) {
        return std::ranges::find(words, w) != words.end();
    };
    EXPECT_TRUE(has("南京")) << "actual: " << join(words);
    EXPECT_TRUE(has("南京市")) << "actual: " << join(words);
    EXPECT_TRUE(has("长江")) << "actual: " << join(words);
    EXPECT_TRUE(has("大桥")) << "actual: " << join(words);
    EXPECT_TRUE(has("长江大桥")) << "actual: " << join(words);
}

TEST(FullSegmentTest, SeparatorsArePreserved) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"我来自北京，他来自上海"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);

    auto has = [&](const std::string &w) {
        return std::ranges::find(words, w) != words.end();
    };
    EXPECT_TRUE(has("来自")) << "actual: " << join(words);
    EXPECT_TRUE(has("北京")) << "actual: " << join(words);
    EXPECT_TRUE(has("上海")) << "actual: " << join(words);
    EXPECT_TRUE(has("，")) << "actual: " << join(words);
}

TEST(FullSegmentTest, WordRangeSlice) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"北京大学"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);

    for (auto &&wr : result) {
        auto slice = wr.slice(runes);
        EXPECT_EQ(slice.size(), wr.size());
        auto word = encode(slice);
        EXPECT_FALSE(word.empty());
    }
}

TEST(FullSegmentTest, WordRangeEquality) {
    auto a = WordRange{0, 3};
    auto b = WordRange{0, 3};
    auto c = WordRange{1, 3};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(FullSegmentTest, AllSingleChars) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"的了是"});
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);

    auto has = [&](const std::string &w) {
        return std::ranges::find(words, w) != words.end();
    };
    EXPECT_TRUE(has("的")) << "actual: " << join(words);
    EXPECT_TRUE(has("了")) << "actual: " << join(words);
    EXPECT_TRUE(has("是")) << "actual: " << join(words);
}

TEST(FullSegmentTest, LongSentence) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto input = std::string_view{"中华人民共和国中央人民政府今天成立了"};
    auto runes = decode(input);
    auto result = cut<FullSegment>(dict, unused_model(), runes);
    auto words = to_strings(runes, result);

    EXPECT_GT(words.size(), 5u);

    auto has = [&](const std::string &w) {
        return std::ranges::find(words, w) != words.end();
    };
    EXPECT_TRUE(has("中华")) << "actual: " << join(words);
    EXPECT_TRUE(has("人民")) << "actual: " << join(words);
    EXPECT_TRUE(has("共和国")) << "actual: " << join(words);
    EXPECT_TRUE(has("中央")) << "actual: " << join(words);
    EXPECT_TRUE(has("政府")) << "actual: " << join(words);
    EXPECT_TRUE(has("成立")) << "actual: " << join(words);
    EXPECT_TRUE(has("今天")) << "actual: " << join(words);
}

TEST(FullSegmentTest, Deterministic) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto runes = decode(std::string_view{"我来自北京邮电大学"});

    auto result1 = cut<FullSegment>(dict, unused_model(), runes);
    auto result2 = cut<FullSegment>(dict, unused_model(), runes);

    ASSERT_EQ(result1.size(), result2.size());
    for (auto i = size_t{0}; i < result1.size(); ++i) {
        EXPECT_EQ(result1[i], result2[i]);
    }
}
