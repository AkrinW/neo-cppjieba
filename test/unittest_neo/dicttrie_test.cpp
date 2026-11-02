#include "../SegmentTestUtils.hpp"
#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/FullSegment.hpp"
#include "neo/algorithm/MPSegment.hpp"
#include "neo/algorithm/MixSegment.hpp"
#include "neo/algorithm/QuerySegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/Dag.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include "test_paths.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

// This fixture uses a different dictionary from the segmentation tests; keep its path local to this file.
constexpr auto DICT_FILE = std::string_view{TEST_DATA_DIR "/extra_dict/jieba.dict.small.utf8"};
inline constexpr auto USER_DICT_FILE = std::string_view{TEST_DATA_DIR "/userdict.utf8"};

namespace {

// Each dictionary validation test owns its input files in an isolated temporary directory.
class DictTrieInputTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = create_temp_directory("neo-dict-trie-");
        write_file("main.dict", "主词 10 n\n基础 20 n\n");
    }

    void TearDown() override {
        if (!directory_.empty()) {
            auto error = std::error_code{};
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    [[nodiscard]] auto file_path(std::string_view name) const -> std::string {
        return path_to_utf8(directory_ / name);
    }

    void write_file(std::string_view name, std::string_view content) const {
        auto output = std::ofstream{directory_ / name, std::ios::binary};
        ASSERT_TRUE(output.is_open());
        output << content;
        output.close();
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(DictTrieInputTest, RejectsMainDictionaryLinesWithMissingFields) {
    for (const auto *const line : {"词\n", "词 10\n"}) {
        ASSERT_NO_FATAL_FAILURE(write_file("main.dict", line));
        EXPECT_THROW((DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian}),
                     LogConfig::Exception);
    }
}

TEST_F(DictTrieInputTest, RejectsMainDictionaryLinesWithExtraFields) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "词 10 n extra\n"));
    EXPECT_THROW((DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian}), LogConfig::Exception);
}

TEST_F(DictTrieInputTest, RejectsNonPositiveMainFrequenciesEvenWhenSumIsPositive) {
    for (const auto *const content : {"词 0 n\n基础 20 n\n", "词 -1 n\n基础 20 n\n"}) {
        ASSERT_NO_FATAL_FAILURE(write_file("main.dict", content));
        EXPECT_THROW((DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian}),
                     LogConfig::Exception);
    }
}

TEST_F(DictTrieInputTest, RejectsMainFrequencyWithTrailingCharacters) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "词 10oops n\n基础 20 n\n"));
    EXPECT_THROW((DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian}), LogConfig::Exception);
}

TEST_F(DictTrieInputTest, RejectsEmptyMainFrequency) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "词  n\n基础 20 n\n"));
    EXPECT_THROW((DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian}), LogConfig::Exception);
}

TEST_F(DictTrieInputTest, RejectsEmptyMainDictionary) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", ""));
    EXPECT_THROW((DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian}), LogConfig::Exception);
}

TEST_F(DictTrieInputTest, AcceptsEmptyTagAndWindowsLineEndings) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "主词 10 \r\n\r\n基础 20 n\r\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto word = trie.find(decode("主词"));
    ASSERT_TRUE(word.has_value());
    EXPECT_TRUE(word.tag.empty());
    EXPECT_FLOAT_EQ(word.weight, std::log(10.0f / 30.0f));
    EXPECT_FALSE(trie.find(decode("缺词")).has_value());
    EXPECT_FALSE(trie.find(decode("")).has_value());
}

TEST_F(DictTrieInputTest, RejectsExtraUserDictionaryFields) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "新词 5 n extra\n"));
    EXPECT_THROW((DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian}),
                 LogConfig::Exception);
}

TEST_F(DictTrieInputTest, RejectsNegativeUserFrequency) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "新词 -1 n\n"));
    EXPECT_THROW((DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian}),
                 LogConfig::Exception);
}

TEST_F(DictTrieInputTest, RejectsUserFrequencyWithTrailingCharacters) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "新词 5oops n\n"));
    EXPECT_THROW((DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian}),
                 LogConfig::Exception);
}

TEST_F(DictTrieInputTest, ZeroUserFrequencyUsesDefaultWeight) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "新词 0 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto word = trie.find(decode("新词"));
    ASSERT_TRUE(word.has_value());
    EXPECT_FLOAT_EQ(word.weight, trie.user_word_default_weight());
}

TEST_F(DictTrieInputTest, RuntimeWordWithFreqMatchesUserDictionaryEntry) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "新词 5 nz\n"));
    const auto loaded =
        DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    auto edited = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};

    edited.add_word_with_freq(decode(std::string_view{"新词"}), 5, PosTag{"nz"});

    const auto expected = loaded.find(decode("新词"));
    const auto actual = edited.find(decode("新词"));
    ASSERT_TRUE(actual.has_value());
    EXPECT_FLOAT_EQ(actual.weight, expected.weight);
    EXPECT_EQ(actual.tag, expected.tag);
}

TEST_F(DictTrieInputTest, RuntimeWordWithoutFreqUsesDefaultWeight) {
    auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};

    trie.add_word(decode(std::string_view{"新词"}), PosTag{"n"});

    const auto word = trie.find(decode("新词"));
    ASSERT_TRUE(word.has_value());
    EXPECT_FLOAT_EQ(word.weight, trie.user_word_default_weight());
    EXPECT_EQ(word.tag, "n");
}

TEST_F(DictTrieInputTest, RuntimeWordRejectsNonPositiveFrequencyWithoutChange) {
    auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};

    EXPECT_THROW(trie.add_word_with_freq(decode(std::string_view{"新词"}), 0, PosTag{"n"}), LogConfig::Exception);
    EXPECT_THROW(trie.add_word_with_freq(decode(std::string_view{"新词"}), -1, PosTag{"n"}), LogConfig::Exception);
    EXPECT_FALSE(trie.find(decode("新词")).has_value());
}

TEST_F(DictTrieInputTest, RuntimeEditsRejectEmptyWords) {
    auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};

    EXPECT_THROW(trie.add_word(Unicode{}, PosTag{"n"}), LogConfig::Exception);
    EXPECT_THROW(trie.add_word_with_freq(Unicode{}, 5, PosTag{"n"}), LogConfig::Exception);
    EXPECT_THROW(trie.remove_word(Unicode{}), LogConfig::Exception);
}

TEST_F(DictTrieInputTest, RemoveWordDoesNotRestoreMainDictionaryWeight) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "主词 3 v\n"));
    auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};

    EXPECT_TRUE(trie.remove_word(decode(std::string_view{"主词"})));

    EXPECT_FALSE(trie.find(decode("主词")).has_value());
    EXPECT_FALSE(trie.remove_word(decode(std::string_view{"主词"})));
    EXPECT_TRUE(trie.find(decode("基础")).has_value());
}

TEST_F(DictTrieInputTest, SingleRuneRuntimeWordTogglesUserWordMarker) {
    auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto word = decode(std::string_view{"龙"});

    trie.add_word(word, PosTag{"n"});
    EXPECT_TRUE(trie.is_user_dict_single_chinese_word(word.front()));

    EXPECT_TRUE(trie.remove_word(word));
    EXPECT_FALSE(trie.is_user_dict_single_chinese_word(word.front()));
}

TEST_F(DictTrieInputTest, CloneKeepsRuntimeEditsIndependent) {
    auto original = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto word = decode(std::string_view{"龙"});
    original.add_word(word, PosTag{"n"});

    auto copy = original.clone();
    ASSERT_TRUE(copy.remove_word(word));

    EXPECT_TRUE(original.find(word).has_value());
    EXPECT_TRUE(original.is_user_dict_single_chinese_word(word.front()));
    EXPECT_FALSE(copy.find(word).has_value());
    EXPECT_FALSE(copy.is_user_dict_single_chinese_word(word.front()));
    EXPECT_FLOAT_EQ(copy.user_word_default_weight(), original.user_word_default_weight());
}

TEST_F(DictTrieInputTest, RejectsUnknownWeightOptionBeforeOpeningDictionary) {
    const auto invalid = static_cast<UserWordWeightOption>(255);
    try {
        const auto trie = DictTrie{file_path("missing.dict"), "", invalid};
        FAIL() << "Expected an invalid weight option error";
    } catch (const LogConfig::Exception &error) {
        EXPECT_NE(std::string_view{error.what()}.find("invalid user word weight option 255"), std::string_view::npos);
    }
}

TEST_F(DictTrieInputTest, MatchTraversalPropagatesVisitorExceptions) {
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("主词");
    const auto fail = [](RuneIndex, const DictUnit &) {
        throw std::runtime_error{"visitor failure"};
    };
    EXPECT_THROW(trie.for_each_match_from(runes, 0, fail), std::runtime_error);
    EXPECT_TRUE(trie.find(std::span<const Rune>{runes}).has_value());
}

TEST_F(DictTrieInputTest, RejectsUnknownWeightOption) {
    const auto invalid = static_cast<UserWordWeightOption>(255);
    EXPECT_THROW((DictTrie{file_path("main.dict"), "", invalid}), LogConfig::Exception);
}

TEST_F(DictTrieInputTest, SingleEntryDictionaryRetainsItsZeroWeightWord) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "词条 10 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto word = trie.find(decode("词条"));
    ASSERT_TRUE(word.has_value());
    EXPECT_FLOAT_EQ(word.weight, 0.0f);
    EXPECT_EQ(word.tag, PosTag{"n"});
    EXPECT_FALSE(trie.find(decode("词")).has_value());
    EXPECT_FALSE(trie.find(decode("外")).has_value());
    EXPECT_EQ(trie.trie().collect_stats().value_count, 1u);
}

TEST_F(DictTrieInputTest, LastMainDictionaryEntryWins) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "词条 10 n\n词条 20 v\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto word = trie.find(decode("词条"));
    ASSERT_TRUE(word.has_value());
    EXPECT_FLOAT_EQ(word.weight, std::log(20.0f / 30.0f));
    EXPECT_EQ(word.tag, PosTag{"v"});
    EXPECT_EQ(trie.trie().collect_stats().value_count, 1u);
}

TEST_F(DictTrieInputTest, UserDictionaryCanOverrideMainWordWithZeroWeight) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "主词 30 v\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto word = trie.find(decode("主词"));
    ASSERT_TRUE(word.has_value());
    EXPECT_FLOAT_EQ(word.weight, 0.0f);
    EXPECT_EQ(word.tag, PosTag{"v"});
    EXPECT_EQ(trie.trie().collect_stats().value_count, 2u);
}

TEST_F(DictTrieInputTest, LastUserDictionaryEntryWins) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "新词 5 n\n新词 30 v\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto word = trie.find(decode("新词"));
    ASSERT_TRUE(word.has_value());
    EXPECT_FLOAT_EQ(word.weight, 0.0f);
    EXPECT_EQ(word.tag, PosTag{"v"});
    EXPECT_EQ(trie.trie().collect_stats().value_count, 3u);
}

TEST_F(DictTrieInputTest, MpSegmentationUsesZeroWeightWithoutFallbackPenalty) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "甲 9 n\n乙 9 n\n稀 1 n\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "甲乙 19 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙");
    const auto words = cut<MPSegment>(trie, unused_model(), runes);
    EXPECT_EQ(to_strings(runes, words), (std::vector<std::string>{"甲乙"}));
}

TEST_F(DictTrieInputTest, MpSegmentationStillEmitsUnknownRunes) {
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("𠮷外");
    const auto words = cut<MPSegment>(trie, unused_model(), runes);
    EXPECT_EQ(to_strings(runes, words), (std::vector<std::string>{"𠮷", "外"}));
}

TEST_F(DictTrieInputTest, SingleEntryWordSurvivesAllSegmentationModes) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "甲乙 1 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{DICT_DIR "/hmm_model.utf8"};
    const auto runes = decode("𠮷，甲乙甲乙。");
    const auto expected = std::vector<WordRange>{{0, 1}, {1, 2}, {2, 4}, {4, 6}, {6, 7}};

    EXPECT_EQ(cut<MPSegment>(trie, unused_model(), runes), expected);
    EXPECT_EQ(cut<FullSegment>(trie, unused_model(), runes), expected);
    EXPECT_EQ(cut<MixSegment<true>>(trie, model, runes), expected);
    EXPECT_EQ(cut<MixSegment<false>>(trie, model, runes), expected);
    EXPECT_EQ(cut<QuerySegment<true>>(trie, model, runes), expected);
    EXPECT_EQ(cut<QuerySegment<false>>(trie, model, runes), expected);
}

TEST_F(DictTrieInputTest, MpSegmentationKeepsLowFrequencyUserWordWithSingleEntryMainDictionary) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "主词 10 n\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "甲乙 1 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙");
    EXPECT_EQ(to_strings(runes, cut<MPSegment>(trie, unused_model(), runes)), (std::vector<std::string>{"甲乙"}));
}

TEST_F(DictTrieInputTest, MpSegmentationPrefersDictionaryWordOverUnknownRunesOnEqualScores) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "主词 2 n\n基础 2 n\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "甲乙 1 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙");
    EXPECT_EQ(to_strings(runes, cut<MPSegment>(trie, unused_model(), runes)), (std::vector<std::string>{"甲乙"}));
}

TEST_F(DictTrieInputTest, MpSegmentationPreservesEqualScoreOrderBetweenKnownWords) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "甲 2 n\n乙 2 n\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "甲乙 1 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙");
    EXPECT_EQ(to_strings(runes, cut<MPSegment>(trie, unused_model(), runes)), (std::vector<std::string>{"甲", "乙"}));
}

TEST_F(DictTrieInputTest, MpSegmentationSelectsHigherProbabilityWhenFloatScoresWouldTie) {
    // The competing frequency products differ by one; float weights collapse their scores into a tie.
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "甲 9999 n\n乙丙 10001 n\n甲乙 10000 n\n丙 10000 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙丙");

    EXPECT_EQ(to_strings(runes, cut<MPSegment>(trie, unused_model(), runes)), (std::vector<std::string>{"甲乙", "丙"}));
}

TEST_F(DictTrieInputTest, MpSegmentationAcceptsPositiveUserWordWeights) {
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "甲乙 60 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙");
    EXPECT_EQ(to_strings(runes, cut<MPSegment>(trie, unused_model(), runes)), (std::vector<std::string>{"甲乙"}));
}

TEST_F(DictTrieInputTest, MpSegmentationPreservesOffsetsAtWordRangeLimit) {
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("外，𠮷");
    constexpr auto limit = std::numeric_limits<RuneIndex>::max();
    auto words = std::vector<WordRange>{};
    auto scratch = detail::SegmentScratch{};
    detail::mp_cut_append(trie, runes, limit - 3, words, scratch);
    EXPECT_EQ(words, (std::vector<WordRange>{{limit - 3, limit - 2}, {limit - 2, limit - 1}, {limit - 1, limit}}));
}

TEST_F(DictTrieInputTest, MatchTraversalReturnsPrefixesInIncreasingRuneEndOrder) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "𠮷 1 n\n𠮷甲 2 v\n𠮷甲乙 3 nt\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("外𠮷甲乙外");
    auto ends = std::vector<RuneIndex>{};
    auto values = std::vector<DictUnit>{};
    trie.for_each_match_from(runes, 1, [&](RuneIndex end, const DictUnit &word) {
        ends.push_back(end);
        values.push_back(word);
    });

    EXPECT_EQ(ends, (std::vector<RuneIndex>{2, 3, 4}));
    ASSERT_EQ(values.size(), 3u);
    EXPECT_FLOAT_EQ(values[0].weight, std::log(1.0f / 6.0f));
    EXPECT_FLOAT_EQ(values[1].weight, std::log(2.0f / 6.0f));
    EXPECT_FLOAT_EQ(values[2].weight, std::log(3.0f / 6.0f));
    EXPECT_EQ(values[0].tag, PosTag{"n"});
    EXPECT_EQ(values[1].tag, PosTag{"v"});
    EXPECT_EQ(values[2].tag, PosTag{"nt"});
}

TEST_F(DictTrieInputTest, MatchTraversalSkipsNonWordPrefixesAndKeepsZeroWeights) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "甲乙 10 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲乙");
    auto ends = std::vector<RuneIndex>{};
    trie.for_each_match_from(runes, 0, [&](RuneIndex end, const DictUnit &word) {
        ends.push_back(end);
        EXPECT_FLOAT_EQ(word.weight, 0.0f);
    });

    EXPECT_EQ(ends, (std::vector<RuneIndex>{2}));
}

TEST_F(DictTrieInputTest, MatchTraversalDoesNotEmitUnknownRuneFallbacks) {
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("𠮷外");
    auto matches = 0;
    trie.for_each_match_from(runes, 0, [&](RuneIndex, const DictUnit &) { ++matches; });
    EXPECT_EQ(matches, 0);
}

TEST_F(DictTrieInputTest, MatchTraversalStopsAtMissingContinuation) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "甲 1 n\n甲乙 1 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    const auto runes = decode("甲外乙");
    auto ends = std::vector<RuneIndex>{};
    trie.for_each_match_from(runes, 0, [&](RuneIndex end, const DictUnit &) { ends.push_back(end); });
    EXPECT_EQ(ends, (std::vector<RuneIndex>{1}));
}

TEST_F(DictTrieInputTest, MatchTraversalEmitsNothingAtTheInputEnd) {
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    auto matches = 0;
    const auto emit = [&](RuneIndex, const DictUnit &) {
        ++matches;
    };
    const auto runes = decode("主词");
    trie.for_each_match_from(runes, static_cast<RuneIndex>(runes.size()), emit);
    trie.for_each_match_from(Unicode{}, 0, emit);
    EXPECT_EQ(matches, 0);
}

TEST_F(DictTrieInputTest, MixUserSingleWordBoundsHmmRunsBetweenDictionaryWords) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "北京 100 n\n上海 100 n\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "研 1 n\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{DICT_DIR "/hmm_model.utf8"};
    const auto runes = decode("北京杭研杭上海");
    const auto result = cut<MixSegment<true>>(trie, model, runes);

    EXPECT_EQ(to_strings(runes, result), (std::vector<std::string>{"北京", "杭", "研", "杭", "上海"}));
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 7}}));
}

TEST_F(DictTrieInputTest, QueryDagIncludesFinalBigramAndTrigramAfterASeparator) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "abcd 1000 eng\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "ab 1 eng\nbc 1 eng\ncd 1 eng\nabc 1 eng\nbcd 1 eng\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{DICT_DIR "/hmm_model.utf8"};
    const auto runes = decode("𠮷，abcd");
    const auto expected = std::vector<WordRange>{{0, 1}, {1, 2}, {2, 4}, {3, 5}, {4, 6}, {2, 5}, {3, 6}, {2, 6}};

    EXPECT_EQ(cut<QuerySegment<true>>(trie, model, runes), expected);
    EXPECT_EQ(cut<QuerySegment<false>>(trie, model, runes), expected);
}

TEST_F(DictTrieInputTest, QueryHmmIncludesFinalBigramAndTrigramAfterASeparator) {
    ASSERT_NO_FATAL_FAILURE(write_file("main.dict", "a 100 eng\nb 100 eng\nc 100 eng\nd 100 eng\n"));
    ASSERT_NO_FATAL_FAILURE(write_file("user.dict", "ab 1 eng\nbc 1 eng\ncd 1 eng\nabc 1 eng\nbcd 1 eng\n"));
    const auto trie = DictTrie{file_path("main.dict"), file_path("user.dict"), UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{DICT_DIR "/hmm_model.utf8"};
    const auto runes = decode("𠮷，abcd");
    const auto expected = std::vector<WordRange>{{0, 1}, {1, 2}, {2, 4}, {3, 5}, {4, 6}, {2, 5}, {3, 6}, {2, 6}};

    EXPECT_EQ(cut<QuerySegment<true>>(trie, model, runes), expected);
}

// ─── Construction ────────────────────────────────────────────────────────────

TEST(DictTrieTest, ConstructMainDictOnly) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    // smoke test: the trie should be non-empty and have sensible statistics
    EXPECT_LT(trie.min_weight(), 0.0f);
    EXPECT_LT(trie.max_weight(), 0.0f);
    EXPECT_GT(trie.freq_sum(), 0.0f);
}

TEST(DictTrieTest, ConstructWithUserDict) {
    auto trie = DictTrie{DICT_FILE, USER_DICT_FILE, UserWordWeightOption::WordWeightMedian};
    EXPECT_GT(trie.freq_sum(), 0.0f);
}

// ─── Weight statistics ───────────────────────────────────────────────────────

TEST(DictTrieTest, WeightOrdering) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    // min ≤ median ≤ max  (all negative since log(freq/sum) < 0)
    EXPECT_LE(trie.min_weight(), trie.median_weight());
    EXPECT_LE(trie.median_weight(), trie.max_weight());
}

TEST(DictTrieTest, MinWeightValue) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    // The old test checked: GetMinWeight() ≈ -15.6479  (within 0.001)
    // float precision: allow slightly wider tolerance
    EXPECT_NEAR(trie.min_weight(), -15.6479f, 0.01f);
}

TEST(DictTrieTest, DefaultWeightIsMedian) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    EXPECT_FLOAT_EQ(trie.user_word_default_weight(), trie.median_weight());
}

TEST(DictTrieTest, UserWordWeightMin) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMin};
    EXPECT_FLOAT_EQ(trie.user_word_default_weight(), trie.min_weight());
}

TEST(DictTrieTest, UserWordWeightMax) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMax};
    EXPECT_FLOAT_EQ(trie.user_word_default_weight(), trie.max_weight());
}

// ─── Find: main dictionary ──────────────────────────────────────────────────

TEST(DictTrieTest, FindExistingWord) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("来到"));
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit.tag, PosTag{"v"});
    EXPECT_NEAR(unit.weight, -8.870f, 0.01f);
}

TEST(DictTrieTest, FindMultiCharWord) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("清华大学"));
    ASSERT_TRUE(unit.has_value());
    // just verify it exists and has a PosTag
    EXPECT_FALSE(unit.tag.empty());
}

TEST(DictTrieTest, FindSingleChar) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("的"));
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit.tag, PosTag{"uj"});
}

TEST(DictTrieTest, FindNonExistentWord) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("不存在的词xyz"));
    EXPECT_FALSE(unit.has_value());
}

TEST(DictTrieTest, FindEmptyString) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    EXPECT_FALSE(trie.find(decode("")).has_value());
}

// ─── Find: user dictionary ──────────────────────────────────────────────────

TEST(DictTrieTest, FindUserDictWordNoFreqNoTag) {
    // userdict.utf8 line: "云计算"  (1 field → default weight, empty tag)
    auto trie = DictTrie{DICT_FILE, USER_DICT_FILE, UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("云计算"));
    ASSERT_TRUE(unit.has_value());
    // weight should equal user_word_default_weight (median)
    EXPECT_FLOAT_EQ(unit.weight, trie.user_word_default_weight());
    EXPECT_TRUE(unit.tag.empty());
}

TEST(DictTrieTest, FindUserDictWordWithTag) {
    // userdict.utf8 line: "蓝翔 nz"  (2 fields → default weight, tag = "nz")
    auto trie = DictTrie{DICT_FILE, USER_DICT_FILE, UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("蓝翔"));
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit.tag, PosTag{"nz"});
    EXPECT_FLOAT_EQ(unit.weight, trie.user_word_default_weight());
}

TEST(DictTrieTest, FindUserDictWordWithFreqAndTag) {
    // userdict.utf8 line: "区块链 10 nz"  (3 fields → freq=10, tag="nz")
    auto trie = DictTrie{DICT_FILE, USER_DICT_FILE, UserWordWeightOption::WordWeightMedian};

    auto unit = trie.find(decode("区块链"));
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit.tag, PosTag{"nz"});
    // weight = log(10 / freq_sum), should be distinctly different from default
    auto expected = std::log(10.0f / trie.freq_sum());
    EXPECT_NEAR(unit.weight, expected, 0.01f);
}

TEST(DictTrieTest, UserDictMaxWeight) {
    auto trie = DictTrie{DICT_FILE, USER_DICT_FILE, UserWordWeightOption::WordWeightMax};

    // "云计算" has no freq → should use max weight as default
    auto unit = trie.find(decode("云计算"));
    ASSERT_TRUE(unit.has_value());
    EXPECT_NEAR(unit.weight, trie.max_weight(), 0.01f);
}

// ─── User dict single Chinese character ──────────────────────────────────────

TEST(DictTrieTest, UserDictSingleChineseWord) {
    // userdict.utf8 contains "A" and "B" as single-char entries
    auto trie = DictTrie{DICT_FILE, USER_DICT_FILE, UserWordWeightOption::WordWeightMedian};

    // 'A' and 'B' are single characters in the user dict
    EXPECT_TRUE(trie.is_user_dict_single_chinese_word(U'A'));
    EXPECT_TRUE(trie.is_user_dict_single_chinese_word(U'B'));
    // a multi-char user word's first char should NOT be in the set
    // '蓝' is part of "蓝翔" — 2-char word, should not be in set
    EXPECT_FALSE(trie.is_user_dict_single_chinese_word(U'蓝'));
    // a character not in user dict at all
    EXPECT_FALSE(trie.is_user_dict_single_chinese_word(U'Z'));
}

// ─── Find with rune span ────────────────────────────────────────────────────

TEST(DictTrieTest, FindByRuneSpan) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};

    auto runes = decode(std::string_view{"来到"});
    auto unit = trie.find(std::span<const Rune>{runes});
    ASSERT_TRUE(unit.has_value());
    EXPECT_EQ(unit.tag, PosTag{"v"});
}

// ─── Find DAG ───────────────────────────────────────────────────────────────

TEST(DictTrieTest, FindDag) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto text = std::string_view{"来到华中科技大学"};
    auto runes = decode(text);
    auto dag = trie.trie().find_dag(runes);

    // "来", "来到", "到", "华", "华中", "中", "科", "科技", "科技大", "科技大学", ...
    EXPECT_EQ(dag.offsets.size(), runes.size() + 1);

    auto edges_0 = dag.get_edges(0); // '来'
    ASSERT_GE(edges_0.size(), 2U);   // '来', '来到'
    EXPECT_EQ(edges_0[0].next_pos, 1U);
    EXPECT_EQ(edges_0[1].next_pos, 2U);

    auto edges_4 = dag.get_edges(4); // '科'
    ASSERT_GE(edges_4.size(), 2U);   // '科', '科技', '科技大学'?
    // check single char has a length
    EXPECT_EQ(edges_4[0].next_pos, 5U);

    // Check that all single chars at least have (i, i+1)
    for (size_t i = 0; i < runes.size(); ++i) {
        auto edges = dag.get_edges(i);
        ASSERT_FALSE(edges.empty());
        EXPECT_EQ(edges[0].next_pos, i + 1);
    }
}

// ─── Dag: structural tests ──────────────────────────────────────────────────

TEST(DagTest, EmptyDag) {
    auto dag = Dag{};
    EXPECT_EQ(dag.size(), 0U);
    EXPECT_TRUE(dag.get_edges(0).empty());

    auto runes = Unicode{};
    EXPECT_EQ(dag.to_string(runes), "");
}

TEST(DagTest, MaximumVertexIndexReturnsNoEdges) {
    const auto dag = Dag{{0, 1}, {{1, -1.0f}}};
    EXPECT_TRUE(dag.get_edges(std::numeric_limits<size_t>::max()).empty());
    EXPECT_TRUE(dag.get_edges(dag.size()).empty());
    EXPECT_TRUE(Dag{}.get_edges(std::numeric_limits<size_t>::max()).empty());
}

TEST(DagTest, VertexWithoutEdgesReturnsAnEmptySpan) {
    const auto dag = Dag{{0, 0}, {}};
    EXPECT_TRUE(dag.get_edges(0).empty());
}

TEST(DagTest, FormatsSupplementaryUnicodeUsingRuneIndices) {
    const auto dag = Dag{{0, 1, 1}, {{2, -1.0f}}};
    const auto runes = decode("𠮷中");
    EXPECT_EQ(dag.to_string(runes, ", "), "𠮷中(-1)");
}

TEST(DagTest, FormatsUnicodeAndEmbeddedNul) {
    const auto dag = Dag{{0, 1, 2, 3}, {{1, -1.0f}, {2, -2.0f}, {3, -3.0f}}};
    const auto runes = Unicode{U'中', U'\0', U'😀'};
    const auto expected = std::string{"中(-1), "} + '\0' + "(-2), 😀(-3)";
    EXPECT_EQ(dag.to_string(runes, ", "), expected);
}

TEST(DagTest, SizeMismatchReturnsEmpty) {
    // Build a DAG for 2 runes but pass 3 runes to to_string → empty
    auto dag = Dag{};
    dag.offsets = {0, 1, 1};
    dag.edges = {{1, 1.0f}};
    EXPECT_EQ(dag.size(), 2U);

    auto runes = decode(std::string_view{"三个字"});
    EXPECT_EQ(dag.to_string(runes), "");
}

TEST(DagTest, SingleCharToString) {
    auto dag = Dag{};
    dag.offsets = {0, 1};
    dag.edges = {{1, -5.0f}};
    EXPECT_EQ(dag.size(), 1U);

    auto runes = decode(std::string_view{"我"});
    EXPECT_EQ(dag.to_string(runes), "我(-5)");
}

TEST(DagTest, TwoCharToString) {
    // Two individual characters, no multi-char edge
    auto dag = Dag{};
    dag.offsets = {0, 1, 2};
    dag.edges = {
        {1, -5.0f}, // pos 0 → pos 1 ("你")
        {2, -5.0f}, // pos 1 → pos 2 ("好")
    };

    auto runes = decode(std::string_view{"你好"});
    EXPECT_EQ(dag.to_string(runes), "你(-5), 好(-5)");
}

TEST(DagTest, MultiEdgeToString) {
    // Two individual chars plus a 2-char edge
    auto dag = Dag{};
    dag.offsets = {0, 2, 3};
    dag.edges = {
        {1, -5.0f}, // pos 0 → pos 1 (single "你")
        {2, -2.0f}, // pos 0 → pos 2 (merged "你好")
        {2, -5.0f}, // pos 1 → pos 2 (single "好")
    };

    auto runes = decode(std::string_view{"你好"});
    // All sub-words dumped: 你, 你好, 好
    auto result = dag.to_string(runes);
    EXPECT_NE(result.find("你(-5)"), std::string::npos);
    EXPECT_NE(result.find("你好(-2)"), std::string::npos);
    EXPECT_NE(result.find("好(-5)"), std::string::npos);
}

TEST(DagTest, ThreeCharToString) {
    auto dag = Dag{};
    dag.offsets = {0, 1, 3, 4};
    dag.edges = {
        {1, -5.0f}, // pos 0 → 1 "我"
        {2, -5.0f}, // pos 1 → 2 "来"
        {3, -3.0f}, // pos 1 → 3 "来了"
        {3, -5.0f}, // pos 2 → 3 "了"
    };

    auto runes = decode(std::string_view{"我来了"});
    auto result = dag.to_string(runes);
    EXPECT_NE(result.find("我(-5)"), std::string::npos);
    EXPECT_NE(result.find("来(-5)"), std::string::npos);
    EXPECT_NE(result.find("来了(-3)"), std::string::npos);
    EXPECT_NE(result.find("了(-5)"), std::string::npos);
}

TEST(DagTest, ZeroWeightToString) {
    // An edge with weight 0.0 (not in dict) is dumped as-is
    auto dag = Dag{};
    dag.offsets = {0, 1, 2};
    dag.edges = {
        {1, 0.0f},  // unknown char at pos 0
        {2, -3.0f}, // known char at pos 1
    };

    auto runes = decode(std::string_view{"x好"});
    auto result = dag.to_string(runes);
    EXPECT_NE(result.find("x(0)"), std::string::npos);
    EXPECT_NE(result.find("好(-3)"), std::string::npos);
}

TEST(DagTest, CustomSeparator) {
    auto dag = Dag{};
    dag.offsets = {0, 1, 2, 3};
    dag.edges = {
        {1, -1.0f},
        {2, -1.0f},
        {3, -1.0f},
    };

    auto runes = decode(std::string_view{"一二三"});
    EXPECT_EQ(dag.to_string(runes, " "), "一(-1) 二(-1) 三(-1)");
    EXPECT_EQ(dag.to_string(runes, " | "), "一(-1) | 二(-1) | 三(-1)");
}

// ─── Dag: to_string with real dictionary ─────────────────────────────────────

TEST(DagTest, ToStringWithDict) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto text = std::string_view{"来到北京清华大学"};
    auto runes = decode(text);
    auto dag = trie.trie().find_dag(runes);

    auto result = dag.to_string(runes);
    EXPECT_FALSE(result.empty());
    // Should contain sub-words with their weights
    EXPECT_NE(result.find("来到("), std::string::npos);
    EXPECT_NE(result.find("清华大学("), std::string::npos);
    EXPECT_NE(result.find("北京("), std::string::npos);
}

TEST(DagTest, ToStringHeSayJump) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto text = std::string_view{"他来到了网易杭研大厦"};
    auto runes = decode(text);
    auto dag = trie.trie().find_dag(runes);

    auto result = dag.to_string(runes);
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("来到("), std::string::npos);
}

TEST(DagTest, ToStringPrint) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    auto text = std::string_view{"南京市长江大桥"};
    auto runes = decode(text);
    auto dag = trie.trie().find_dag(runes);

    auto result = dag.to_string(runes);
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("南京市("), std::string::npos);
    std::cout << dag.to_string(runes) << std::endl;
}

// ─── Trie accessor ──────────────────────────────────────────────────────────

TEST(DictTrieTest, TrieAccessor) {
    auto trie = DictTrie{DICT_FILE, "", UserWordWeightOption::WordWeightMedian};
    const auto &t = trie.trie();
    EXPECT_FALSE(t.empty());
    EXPECT_GT(t.node_count(), 0u);
}

TEST_F(DictTrieInputTest, ReusedDagReplacesPreviousMatchesIncludingEmptyInput) {
    const auto trie = DictTrie{file_path("main.dict"), "", UserWordWeightOption::WordWeightMedian};
    auto dag = Dag{};
    for (const auto *const text : {"甲乙甲乙甲乙", "外", "", "甲乙"}) {
        const auto runes = decode(std::string_view{text});
        const auto expected = trie.find_dag(runes);
        trie.find_dag_into(runes, dag);
        EXPECT_EQ(dag.offsets, expected.offsets);
        ASSERT_EQ(dag.edges.size(), expected.edges.size());
        for (auto i = size_t{0}; i < dag.edges.size(); ++i) {
            EXPECT_EQ(dag.edges[i].next_pos, expected.edges[i].next_pos);
            EXPECT_EQ(dag.edges[i].weight, expected.edges[i].weight);
        }
    }
}
