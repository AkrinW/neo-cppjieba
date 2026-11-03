#include "cppjieba/KeywordExtractor.hpp"
#include "cppjieba/TextRankExtractor.hpp"
#include "gtest/gtest.h"
#include "neo/Jieba.hpp"
#include "neo/KeywordExtractor.hpp"

#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <ios>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

inline constexpr auto DICT_FILE = std::string_view{DICT_DIR "/jieba.dict.utf8"};
inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};
inline constexpr auto USER_DICT_FILE = std::string_view{TEST_DATA_DIR "/userdict.utf8"};
inline constexpr auto IDF_FILE = std::string_view{DICT_DIR "/idf.utf8"};
inline constexpr auto STOP_WORDS_FILE = std::string_view{DICT_DIR "/stop_words.utf8"};

constexpr auto all = std::numeric_limits<size_t>::max();

struct Comparable {
    std::string word;
    double weight;
    std::vector<size_t> offsets;

    auto operator==(const Comparable &) const -> bool = default;
};

// Upstream breaks score ties by partial_sort's unspecified order, so compare complete results in a fixed order.
// First offsets identify words even where upstream leaves the word empty.
auto normalized(std::vector<Comparable> words) -> std::vector<Comparable> {
    std::ranges::sort(words, [](const Comparable &lhs, const Comparable &rhs) {
        return std::tie(rhs.weight, lhs.offsets) < std::tie(lhs.weight, rhs.offsets);
    });
    return words;
}

// Upstream TextRank sets Word::word only for graph nodes, so a word without edges comes back empty
// with weight 0. neo-cppjieba reports the word; its weight and offsets must still match.
auto fill_isolated_words(std::vector<Comparable> &expected, const std::vector<Comparable> &actual) -> void {
    for (auto i = size_t{0}; i < std::min(expected.size(), actual.size()); ++i) {
        if (expected[i].word.empty() && expected[i].weight == 0.0) {
            expected[i].word = actual[i].word;
        }
    }
}

template <typename Word>
auto from_upstream(const std::vector<Word> &words) -> std::vector<Comparable> {
    auto result = std::vector<Comparable>{};
    for (const auto &word : words) {
        result.push_back({word.word, word.weight, word.offsets});
    }
    return normalized(std::move(result));
}

auto from_neo(const std::vector<neo_cppjieba::Keyword<char>> &keywords) -> std::vector<Comparable> {
    auto result = std::vector<Comparable>{};
    for (const auto &keyword : keywords) {
        auto offsets = std::vector<size_t>{};
        for (const auto &position : keyword.positions) {
            offsets.push_back(position.source.begin);
        }
        result.push_back({keyword.word, keyword.weight, std::move(offsets)});
    }
    return normalized(std::move(result));
}

auto read_text(const std::string &path) -> std::string {
    auto file = std::ifstream{path, std::ios::binary};
    auto buffer = std::stringstream{};
    buffer << file.rdbuf();
    return buffer.str();
}

// Every corpus line, each corpus as one document, and the legacy fixture sentences.
auto corpus() -> std::vector<std::string> {
    auto texts = std::vector<std::string>{
        "你好世界世界而且而且",
        "我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。",
        "一部iPhone6",
        "蓝翔优秀毕业生",
        "他来到了网易杭研大厦。小明硕士毕业于中国科学院计算所，后在日本京都大学深造",
    };
    for (const auto *path :
         {TEST_DATA_DIR "/weicheng.utf8", TEST_DATA_DIR "/review.100", TEST_DATA_DIR "/testlines.utf8"}) {
        const auto text = read_text(path);
        EXPECT_FALSE(text.empty()) << path;
        texts.push_back(text);
        auto lines = std::istringstream{text};
        for (auto line = std::string{}; std::getline(lines, line);) {
            texts.push_back(line);
        }
    }
    return texts;
}

class KeywordCompareTest : public testing::TestWithParam<bool> {
protected:
    [[nodiscard]]
    auto user_dict() const -> std::string_view {
        return GetParam() ? USER_DICT_FILE : std::string_view{};
    }
};

} // namespace

TEST_P(KeywordCompareTest, TfIdfMatchesCppJieba) {
    const auto upstream =
        cppjieba::KeywordExtractor{std::string{DICT_FILE}, std::string{HMM_MODEL_FILE}, std::string{IDF_FILE},
                                   std::string{STOP_WORDS_FILE}, std::string{user_dict()}};
    const auto jieba = neo_cppjieba::Jieba{DICT_FILE, HMM_MODEL_FILE, user_dict()};
    const auto extractor = neo_cppjieba::TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    auto workspace = neo_cppjieba::KeywordWorkspace{};
    auto keywords = std::vector<neo_cppjieba::Keyword<char>>{};
    for (const auto &text : corpus()) {
        auto expected = std::vector<cppjieba::KeywordExtractor::Word>{};
        upstream.Extract(text, expected, all);
        extractor.extract_into(jieba, text, all, neo_cppjieba::TfIdfOptions{}, keywords, workspace);
        ASSERT_EQ(from_neo(keywords), from_upstream(expected)) << text;
    }
}

TEST_P(KeywordCompareTest, TextRankMatchesCppJieba) {
    const auto upstream = cppjieba::TextRankExtractor{std::string{DICT_FILE}, std::string{HMM_MODEL_FILE},
                                                      std::string{STOP_WORDS_FILE}, std::string{user_dict()}};
    const auto jieba = neo_cppjieba::Jieba{DICT_FILE, HMM_MODEL_FILE, user_dict()};
    const auto extractor = neo_cppjieba::TextRankExtractor{STOP_WORDS_FILE};
    auto workspace = neo_cppjieba::KeywordWorkspace{};
    auto keywords = std::vector<neo_cppjieba::Keyword<char>>{};
    for (const auto &text : corpus()) {
        auto upstream_words = std::vector<cppjieba::TextRankExtractor::Word>{};
        upstream.Extract(text, upstream_words, all);
        extractor.extract_into(jieba, text, all, neo_cppjieba::TextRankOptions{}, keywords, workspace);
        const auto actual = from_neo(keywords);
        auto expected = from_upstream(upstream_words);
        fill_isolated_words(expected, actual);
        ASSERT_EQ(actual, expected) << text;
    }
}

TEST_P(KeywordCompareTest, TextRankOptionsMatchCppJieba) {
    const auto upstream = cppjieba::TextRankExtractor{std::string{DICT_FILE}, std::string{HMM_MODEL_FILE},
                                                      std::string{STOP_WORDS_FILE}, std::string{user_dict()}};
    const auto jieba = neo_cppjieba::Jieba{DICT_FILE, HMM_MODEL_FILE, user_dict()};
    const auto text = read_text(TEST_DATA_DIR "/weicheng.utf8");
    // Both libraries take the TextRank parameters with each call.
    const auto extractor = neo_cppjieba::TextRankExtractor{STOP_WORDS_FILE};
    for (const auto [window, iterations] :
         std::array{std::pair<size_t, size_t>{2, 1}, std::pair<size_t, size_t>{8, 30}}) {
        const auto options = neo_cppjieba::TextRankOptions{.window = window, .iterations = iterations};
        auto expected = std::vector<cppjieba::TextRankExtractor::Word>{};
        upstream.Extract(text, expected, all, window, iterations);
        ASSERT_EQ(from_neo(extractor.extract(jieba, text, all, options)), from_upstream(expected))
            << window << ' ' << iterations;
    }
}

INSTANTIATE_TEST_SUITE_P(UserDict, KeywordCompareTest, testing::Bool());
