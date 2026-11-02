#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/MixSegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include "test_paths.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

inline constexpr auto DICT_FILE = std::string_view{DICT_DIR "/jieba.dict.utf8"};
inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};

TEST(MixSegmentNeoTest, EmptyInput) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = Unicode{};
    auto result = cut<MixSegment<true>>(dict, model, runes);
    EXPECT_TRUE(result.empty());
}

TEST(MixSegmentNeoTest, SingleChar) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"我"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    ASSERT_EQ(words.size(), 1U);
    EXPECT_EQ(words[0], "我");
}

TEST(MixSegmentNeoTest, SpanSubrangeUsesLocalOffsets) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"甲我来自北京邮电大学乙"});
    auto span = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    auto result = cut<MixSegment<>>(dict, model, span);
    auto words = to_strings(span, result);

    EXPECT_EQ(words, std::vector<std::string>({"我", "来自", "北京邮电大学"}));
    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result.front().begin, 0u);
    EXPECT_EQ(result.back().end, static_cast<RuneIndex>(span.size()));
}

TEST(MixSegmentNeoTest, ClassicSentence) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"我来自北京邮电大学"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    auto expected = std::vector<std::string>{"我", "来自", "北京邮电大学"};
    EXPECT_EQ(words, expected) << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, HangyanBuilding) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"他来到了网易杭研大厦"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    EXPECT_EQ(join(words), "他/来到/了/网易/杭研/大厦") << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, HangyanBuildingNoHMM) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"他来到了网易杭研大厦"});
    auto result = cut<MixSegment<false>>(dict, model, runes);
    auto words = to_strings(runes, result);

    EXPECT_EQ(join(words), "他/来到/了/网易/杭/研/大厦") << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, HmmRunsResumeTheMpRouteAcrossDictionaryWords) {
    const auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode("杭研北京杭研上海杭研");
    const auto result = cut<MixSegment<true>>(dict, model, runes);

    EXPECT_EQ(to_strings(runes, result), (std::vector<std::string>{"杭研", "北京", "杭研", "上海", "杭研"}));
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 2}, {2, 4}, {4, 6}, {6, 8}, {8, 10}}));
}

TEST(MixSegmentNeoTest, NoHmmPreservesRuneOffsetsAroundConsecutiveSeparators) {
    const auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode(std::string_view{"甲 \t杭研，，北京。𠮷😀\n乙"});
    const auto input = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    const auto result = cut<MixSegment<false>>(dict, model, input);

    EXPECT_EQ(to_strings(input, result),
              (std::vector<std::string>{" ", "\t", "杭", "研", "，", "，", "北京", "。", "𠮷", "😀", "\n"}));
    EXPECT_EQ(result, (std::vector<WordRange>{
                          {0, 1},
                          {1, 2},
                          {2, 3},
                          {3, 4},
                          {4, 5},
                          {5, 6},
                          {6, 8},
                          {8, 9},
                          {9, 10},
                          {10, 11},
                          {11, 12},
                      }));
}

TEST(MixSegmentNeoTest, NoHmmEmptyInputClearsReusedOutput) {
    const auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    auto result = std::vector<WordRange>{{0, 1}};
    auto scratch = detail::SegmentScratch{};

    MixSegment<false>::cut_into(dict, model, std::span<const Rune>{}, result, scratch);

    EXPECT_TRUE(result.empty());
}

TEST(MixSegmentNeoTest, SharedScratchPreservesOutputsWhenHmmModeChanges) {
    const auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode(std::string_view{"杭研。杭研"});
    const auto with_hmm = std::vector<WordRange>{{0, 2}, {2, 3}, {3, 5}};
    const auto without_hmm = std::vector<WordRange>{{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}};
    auto result = std::vector<WordRange>{};
    auto scratch = detail::SegmentScratch{};

    MixSegment<true>::cut_into(dict, model, runes, result, scratch);
    EXPECT_EQ(result, with_hmm);
    MixSegment<false>::cut_into(dict, model, runes, result, scratch);
    EXPECT_EQ(result, without_hmm);
    MixSegment<true>::cut_into(dict, model, runes, result, scratch);
    EXPECT_EQ(result, with_hmm);
}

TEST(MixSegmentNeoTest, UnicodeOverloadWithSeparators) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string_view{"我来自北京邮电大学。。。学号123456，用AK47"};
    auto runes = decode(sentence);
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);
    auto expected =
        std::vector<std::string>{"我", "来自", "北京邮电大学", "。", "。", "。", "学号", "123456", "，", "用", "AK47"};
    EXPECT_EQ(words, expected) << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, HmmRunsPreserveRuneOffsetsAroundConsecutiveSeparators) {
    const auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{HMM_MODEL_FILE};
    const auto runes = decode(std::string_view{"甲 \t杭研qzx987，，3.14。\n𠮷乙"});
    const auto input = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    const auto result = cut<MixSegment<true>>(dict, model, input);

    EXPECT_EQ(to_strings(input, result),
              (std::vector<std::string>{" ", "\t", "杭研", "qzx987", "，", "，", "3.14", "。", "\n", "𠮷"}));
    EXPECT_EQ(result, (std::vector<WordRange>{
                          {0, 1},
                          {1, 2},
                          {2, 4},
                          {4, 10},
                          {10, 11},
                          {11, 12},
                          {12, 16},
                          {16, 17},
                          {17, 18},
                          {18, 19},
                      }));
}

TEST(MixSegmentNeoTest, BChaoTShirt) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"B超 T恤"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    EXPECT_EQ(join(words), "B超/ /T恤") << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, Unicode32Emoji) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"天气很好，🙋 我们去郊游。"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    EXPECT_EQ(join(words), "天气/很/好/，/🙋/ /我们/去/郊游/。") << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, WordRangeContiguous) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"小明硕士毕业于中国科学院计算所"});
    auto result = cut<MixSegment<>>(dict, model, runes);

    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result.front().begin, 0u);

    for (size_t i = 1; i < result.size(); ++i) {
        EXPECT_EQ(result[i].begin, result[i - 1].end) << "gap between word " << (i - 1) << " and " << i;
    }
    EXPECT_EQ(result.back().end, static_cast<RuneIndex>(runes.size()));
}

TEST(MixSegmentNeoTest, ReconstructsOriginal) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string{"南京市长江大桥，欢迎你来参观游览。"};
    auto runes = decode(sentence);
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    auto reconstructed = std::string{};
    for (auto &&w : words) {
        reconstructed += w;
    }
    EXPECT_EQ(reconstructed, sentence);
}

TEST(MixSegmentNeoTest, NanjingBridge) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"南京市长江大桥"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    EXPECT_EQ(join(words), "南京市/长江大桥") << "actual: " << join(words);
}

TEST(MixSegmentNeoTest, PureASCII) {
    auto dict = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"IBM,3.14"});
    auto result = cut<MixSegment<>>(dict, model, runes);
    auto words = to_strings(runes, result);

    EXPECT_EQ(join(words), "IBM/,/3.14") << "actual: " << join(words);
}
