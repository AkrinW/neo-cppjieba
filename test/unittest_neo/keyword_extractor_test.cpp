#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/KeywordExtractor.hpp"
#include "neo/Utf.hpp"
#include "neo/value/DictSource.hpp"

#include "test_paths.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace neo_cppjieba;

namespace {

inline constexpr auto SMALL_DICT_FILE = std::string_view{TEST_DATA_DIR "/extra_dict/jieba.dict.small.utf8"};
inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};
inline constexpr auto USER_DICT_FILE = std::string_view{TEST_DATA_DIR "/userdict.utf8"};
inline constexpr auto IDF_FILE = std::string_view{DICT_DIR "/idf.utf8"};
inline constexpr auto STOP_WORDS_FILE = std::string_view{DICT_DIR "/stop_words.utf8"};

constexpr auto all = std::numeric_limits<size_t>::max();

auto small_jieba() -> const Jieba & {
    static const auto jieba = Jieba{SMALL_DICT_FILE, HMM_MODEL_FILE, ""};
    return jieba;
}

auto user_jieba() -> const Jieba & {
    static const auto jieba = Jieba{SMALL_DICT_FILE, HMM_MODEL_FILE, USER_DICT_FILE};
    return jieba;
}

// Format keywords as the legacy CppJieba tests print KeywordExtractor::Word: UTF-8 byte offsets, six digits.
auto format_keywords(const std::vector<Keyword<char>> &keywords) -> std::string {
    auto out = std::string{"["};
    for (const auto &keyword : keywords) {
        auto offsets = std::string{};
        for (const auto &position : keyword.positions) {
            offsets += std::format("{}{}", offsets.empty() ? "" : ", ", position.source.begin);
        }
        out += std::format("{}{{\"word\": \"{}\", \"offset\": [{}], \"weight\": {:.6}}}", out.size() > 1 ? ", " : "",
                           keyword.word, offsets, keyword.weight);
    }
    return out + "]";
}

} // namespace

// Expected values are CppJieba's keyword_extractor_test.cpp fixtures.
TEST(KeywordExtractorTest, TfIdfMatchesCppJiebaFixtures) {
    const auto extractor = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    EXPECT_EQ(
        format_keywords(extractor.extract(small_jieba(), std::string_view{"你好世界世界而且而且"}, 5, TfIdfOptions{})),
        R"([{"word": "世界", "offset": [6, 12], "weight": 8.73506}, )"
        R"({"word": "你好", "offset": [0], "weight": 7.95788}])");
    EXPECT_EQ(
        format_keywords(extractor.extract(
            small_jieba(),
            std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"}, 5,
            TfIdfOptions{})),
        R"([{"word": "CEO", "offset": [93], "weight": 11.7392}, {"word": "升职", "offset": [72], "weight": 10.8562}, )"
        R"({"word": "加薪", "offset": [78], "weight": 10.6426}, )"
        R"({"word": "手扶拖拉机", "offset": [21], "weight": 10.0089}, )"
        R"({"word": "巅峰", "offset": [111], "weight": 9.49396}])");
    EXPECT_EQ(format_keywords(extractor.extract(small_jieba(), std::string_view{"一部iPhone6"}, 5, TfIdfOptions{})),
              R"([{"word": "iPhone6", "offset": [6], "weight": 11.7392}, )"
              R"({"word": "一部", "offset": [0], "weight": 6.47592}])");

    const auto user = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    EXPECT_EQ(format_keywords(user.extract(user_jieba(), std::string_view{"蓝翔优秀毕业生"}, 5, TfIdfOptions{})),
              R"([{"word": "蓝翔", "offset": [0], "weight": 11.7392}, )"
              R"({"word": "毕业生", "offset": [12], "weight": 8.13549}, )"
              R"({"word": "优秀", "offset": [6], "weight": 6.78347}])");
}

// Expected values are CppJieba's textrank_test.cpp fixtures.
TEST(KeywordExtractorTest, TextRankMatchesCppJiebaFixtures) {
    const auto extractor = TextRankExtractor{STOP_WORDS_FILE};
    EXPECT_EQ(format_keywords(
                  extractor.extract(small_jieba(), std::string_view{"你好世界世界而且而且"}, 5, TextRankOptions{})),
              R"([{"word": "世界", "offset": [6, 12], "weight": 1}, )"
              R"({"word": "你好", "offset": [0], "weight": 0.519787}])");
    EXPECT_EQ(
        format_keywords(extractor.extract(
            small_jieba(),
            std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上 CEO，走上人生巅峰"}, 5,
            TextRankOptions{})),
        R"([{"word": "当上", "offset": [87], "weight": 1}, {"word": "不用", "offset": [48], "weight": 0.989848}, )"
        R"({"word": "多久", "offset": [54], "weight": 0.985126}, {"word": "加薪", "offset": [78], "weight": 0.983046}, )"
        R"({"word": "升职", "offset": [72], "weight": 0.980278}])");
    EXPECT_EQ(format_keywords(extractor.extract(small_jieba(), std::string_view{"一部iPhone6"}, 5, TextRankOptions{})),
              R"([{"word": "一部", "offset": [0], "weight": 1}, )"
              R"({"word": "iPhone6", "offset": [6], "weight": 0.996126}])");

    const auto user = TextRankExtractor{STOP_WORDS_FILE};
    EXPECT_EQ(format_keywords(user.extract(user_jieba(), std::string_view{"蓝翔优秀毕业生"}, 5, TextRankOptions{})),
              R"([{"word": "蓝翔", "offset": [0], "weight": 1}, )"
              R"({"word": "毕业生", "offset": [12], "weight": 0.996685}, )"
              R"({"word": "优秀", "offset": [6], "weight": 0.992994}])");
}

TEST(KeywordExtractorTest, TopNAndEmptyInput) {
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    constexpr auto text = std::string_view{"你好世界世界而且而且"};

    EXPECT_TRUE(tfidf.extract(small_jieba(), text, 0, TfIdfOptions{}).empty());
    EXPECT_TRUE(textrank.extract(small_jieba(), text, 0, TextRankOptions{}).empty());
    EXPECT_TRUE(tfidf.extract(small_jieba(), std::string_view{}, 5, TfIdfOptions{}).empty());
    EXPECT_TRUE(textrank.extract(small_jieba(), std::string_view{}, 5, TextRankOptions{}).empty());
    ASSERT_EQ(tfidf.extract(small_jieba(), text, 1, TfIdfOptions{}).size(), 1U);
    EXPECT_EQ(tfidf.extract(small_jieba(), text, 1, TfIdfOptions{})[0].word, "世界");
    EXPECT_EQ(tfidf.extract(small_jieba(), text, all, TfIdfOptions{}).size(), 2U);

    // Only stop words and one-rune words: nothing is a candidate.
    EXPECT_TRUE(tfidf.extract(small_jieba(), std::string_view{"的了，。"}, 5, TfIdfOptions{}).empty());
    EXPECT_TRUE(textrank.extract(small_jieba(), std::string_view{"的了，。"}, 5, TextRankOptions{}).empty());
}

// A lone candidate has no TextRank edges, so it keeps CppJieba's zero score.
TEST(KeywordExtractorTest, TextRankWithoutEdges) {
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    const auto keywords = textrank.extract(small_jieba(), std::string_view{"世界"}, 5, TextRankOptions{});
    ASSERT_EQ(keywords.size(), 1U);
    EXPECT_EQ(keywords[0].word, "世界");
    EXPECT_EQ(keywords[0].weight, 0.0);

    const auto narrow = TextRankOptions{.window = 1};
    for (const auto &keyword : textrank.extract(small_jieba(), std::string_view{"你好世界世界而且而且"}, 5, narrow)) {
        EXPECT_EQ(keyword.weight, 0.0);
    }
}

TEST(KeywordExtractorTest, PositionsUseInputCodeUnits) {
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    constexpr auto utf8 =
        std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};
    const auto runes = Utf::decode(utf8);
    const auto utf16 = Utf::encode<char16_t>(runes);
    const auto utf32 = Utf::encode<char32_t>(runes);

    const auto check_encoding = [&](const auto &extractor, const auto &options) {
        const auto expected = extractor.extract(small_jieba(), utf8, all, options);
        const auto wide16 = extractor.extract(small_jieba(), utf16, all, options);
        const auto wide32 = extractor.extract(small_jieba(), utf32, all, options);
        ASSERT_EQ(wide16.size(), expected.size());
        ASSERT_EQ(wide32.size(), expected.size());
        for (auto i = size_t{0}; i < expected.size(); ++i) {
            EXPECT_EQ(Utf::encode<char>(Utf::decode(wide16[i].word)), expected[i].word);
            EXPECT_EQ(Utf::encode<char>(Utf::decode(wide32[i].word)), expected[i].word);
            EXPECT_EQ(wide16[i].weight, expected[i].weight);
            EXPECT_EQ(wide32[i].weight, expected[i].weight);
            ASSERT_EQ(wide16[i].positions.size(), expected[i].positions.size());
            for (auto j = size_t{0}; j < expected[i].positions.size(); ++j) {
                const auto &position = expected[i].positions[j];
                EXPECT_EQ(position.source.slice(utf8), expected[i].word);
                EXPECT_EQ(wide16[i].positions[j].runes, position.runes);
                EXPECT_EQ(wide32[i].positions[j].runes, position.runes);
                EXPECT_EQ(wide16[i].positions[j].source.slice(std::u16string_view{utf16}), wide16[i].word);
                EXPECT_EQ(wide32[i].positions[j].source.begin, position.runes.begin);
            }
        }
    };
    check_encoding(tfidf, TfIdfOptions{});
    check_encoding(textrank, TextRankOptions{});
}

TEST(KeywordExtractorTest, WorkspaceReuseMatchesFreshCalls) {
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    const auto inputs = std::vector<std::string_view>{
        "我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。", "你好世界世界而且而且", "",
        "一部iPhone6"};

    auto workspace = KeywordWorkspace{};
    auto out = std::vector<Keyword<char>>{};
    for (const auto input : inputs) {
        tfidf.extract_into(small_jieba(), input, all, TfIdfOptions{}, out, workspace);
        EXPECT_EQ(format_keywords(out), format_keywords(tfidf.extract(small_jieba(), input, all, TfIdfOptions{})));
        textrank.extract_into(small_jieba(), input, 3, TextRankOptions{}, out, workspace);
        EXPECT_EQ(format_keywords(out), format_keywords(textrank.extract(small_jieba(), input, 3, TextRankOptions{})));
    }
    workspace.release();
    tfidf.extract_into(small_jieba(), inputs[1], all, TfIdfOptions{}, out, workspace);
    EXPECT_EQ(format_keywords(out), format_keywords(tfidf.extract(small_jieba(), inputs[1], all, TfIdfOptions{})));
}

// Blank lines in an IDF table or a stop-word list define nothing and leave the average IDF unchanged.
TEST(KeywordExtractorTest, BlankResourceLinesAreSkipped) {
    const auto compact = TfIdfExtractor{DictSource::memory("世界 8.0\n你好 2.0\n"), DictSource::memory("而且\n")};
    const auto spaced =
        TfIdfExtractor{DictSource::memory("\n世界 8.0\n\n你好 2.0\n\n"), DictSource::memory("\n而且\n\n")};
    constexpr auto text = std::string_view{"你好世界世界而且而且计算"};

    const auto expected = compact.extract(small_jieba(), text, all, TfIdfOptions{});
    EXPECT_TRUE(std::ranges::none_of(expected, [](const Keyword<char> &keyword) { return keyword.word == "而且"; }));
    EXPECT_EQ(format_keywords(spaced.extract(small_jieba(), text, all, TfIdfOptions{})), format_keywords(expected));
}

TEST(KeywordExtractorTest, RejectsInvalidResources) {
    EXPECT_ANY_THROW((TfIdfExtractor{DICT_DIR "/missing_idf.utf8", STOP_WORDS_FILE}));
    // The main dictionary has three fields per line, so it is not a valid IDF table.
    EXPECT_ANY_THROW((TfIdfExtractor{SMALL_DICT_FILE, STOP_WORDS_FILE}));
}

// Options are checked when a call receives them; a rejected call leaves the extractor usable.
TEST(KeywordExtractorTest, RejectsInvalidTextRankOptionsPerCall) {
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    constexpr auto text = std::string_view{"你好世界世界而且而且"};
    auto workspace = KeywordWorkspace{};
    auto out = std::vector<Keyword<char>>{};
    for (const auto damping : {1.5, -0.1, std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW(textrank.extract_into(small_jieba(), text, 5, TextRankOptions{.damping = damping}, out, workspace),
                     LogConfig::Exception);
    }
    EXPECT_FALSE(textrank.extract(small_jieba(), text, 5, TextRankOptions{.damping = 1.0}).empty());
    EXPECT_EQ(textrank.extract(small_jieba(), text, 5, TextRankOptions{}).size(), 2U);
}

TEST(KeywordExtractorTest, ReportsLookupTags) {
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    constexpr auto text =
        std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};
    for (const auto &keywords : {tfidf.extract(small_jieba(), text, all, TfIdfOptions{}),
                                 textrank.extract(small_jieba(), text, all, TextRankOptions{})}) {
        ASSERT_FALSE(keywords.empty());
        for (const auto &keyword : keywords) {
            EXPECT_EQ(keyword.tag, small_jieba().lookup_tag(keyword.word)) << keyword.word;
        }
    }
    const auto english = tfidf.extract(small_jieba(), std::string_view{"一部iPhone6"}, 1, TfIdfOptions{});
    ASSERT_EQ(english.size(), 1U);
    EXPECT_EQ(english[0].tag, pos::eng);
}

// TF-IDF scores each word independently, so filtering by tag only removes words from the unfiltered result.
TEST(KeywordExtractorTest, TfIdfAllowedTagsFilterCandidates) {
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto allowed = std::vector{pos::n, pos::v, pos::vn};
    const auto filtered = TfIdfOptions{.allowed_tags = allowed};
    constexpr auto text =
        std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};

    auto expected = tfidf.extract(small_jieba(), text, all, TfIdfOptions{});
    std::erase_if(expected, [&](const Keyword<char> &keyword) {
        return std::ranges::find(allowed, keyword.tag) == allowed.end();
    });
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(format_keywords(tfidf.extract(small_jieba(), text, all, filtered)), format_keywords(expected));
    EXPECT_LT(expected.size(), tfidf.extract(small_jieba(), text, all, TfIdfOptions{}).size());

    EXPECT_TRUE(tfidf.extract(small_jieba(), text, all, TfIdfOptions{.allowed_tags = {PosTag{"zz"}}}).empty());
}

// Removed words leave the TextRank graph entirely, so the remaining words are ranked among themselves.
TEST(KeywordExtractorTest, TextRankAllowedTagsFilterCandidates) {
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    const auto python =
        TextRankOptions{.allowed_tags = std::vector<PosTag>(python_textrank_tags.begin(), python_textrank_tags.end())};
    constexpr auto text =
        std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};

    const auto keywords = textrank.extract(small_jieba(), text, all, python);
    ASSERT_FALSE(keywords.empty());
    EXPECT_EQ(keywords[0].weight, 1.0);
    for (const auto &keyword : keywords) {
        EXPECT_NE(std::ranges::find(python_textrank_tags, keyword.tag), python_textrank_tags.end()) << keyword.word;
    }
    for (const auto &keyword : textrank.extract(small_jieba(), text, all, TextRankOptions{})) {
        const auto allowed = std::ranges::find(python_textrank_tags, keyword.tag) != python_textrank_tags.end();
        const auto kept = std::ranges::any_of(keywords, [&](const Keyword<char> &k) { return k.word == keyword.word; });
        EXPECT_EQ(kept, allowed) << keyword.word;
    }
}

// Extractors hold no Jieba, so one extractor follows whichever dictionary each call passes in.
// MIX recovers 蓝翔 through HMM either way; only the edited dictionary gives it the runtime tag.
TEST(KeywordExtractorTest, ExtractorUsesTheJiebaPassedToEachCall) {
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    auto edited = small_jieba().clone();
    edited.add_word("蓝翔", PosTag{"zz"});
    constexpr auto text = std::string_view{"蓝翔优秀毕业生"};
    const auto has_runtime_tag = [](const std::vector<Keyword<char>> &keywords) {
        return std::ranges::any_of(
            keywords, [](const Keyword<char> &keyword) { return keyword.word == "蓝翔" && keyword.tag == "zz"; });
    };

    EXPECT_FALSE(has_runtime_tag(tfidf.extract(small_jieba(), text, all, TfIdfOptions{})));
    EXPECT_FALSE(has_runtime_tag(textrank.extract(small_jieba(), text, all, TextRankOptions{})));
    EXPECT_TRUE(has_runtime_tag(tfidf.extract(edited, text, all, TfIdfOptions{})));
    EXPECT_TRUE(has_runtime_tag(textrank.extract(edited, text, all, TextRankOptions{})));
}

TEST(KeywordExtractorTest, ExtractorsAreMoveOnly) {
    static_assert(!std::is_copy_constructible_v<TfIdfExtractor>
                  && std::is_nothrow_move_constructible_v<TfIdfExtractor>);
    static_assert(!std::is_copy_constructible_v<TextRankExtractor>
                  && std::is_nothrow_move_constructible_v<TextRankExtractor>);
}

TEST(KeywordExtractorTest, ClonesExtractTheSameKeywords) {
    const auto text =
        std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};
    const auto tfidf = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto tfidf_options = TfIdfOptions{.allowed_tags = {pos::n, pos::v}};
    const auto tfidf_clone = tfidf.clone();
    EXPECT_EQ(format_keywords(tfidf_clone.extract(small_jieba(), text, 5, tfidf_options)),
              format_keywords(tfidf.extract(small_jieba(), text, 5, tfidf_options)));
    EXPECT_EQ(tfidf_clone.idf_average(), tfidf.idf_average());

    const auto textrank = TextRankExtractor{STOP_WORDS_FILE};
    const auto textrank_options = TextRankOptions{.window = 3};
    const auto textrank_clone = textrank.clone();
    EXPECT_EQ(format_keywords(textrank_clone.extract(small_jieba(), text, 5, textrank_options)),
              format_keywords(textrank.extract(small_jieba(), text, 5, textrank_options)));
}
