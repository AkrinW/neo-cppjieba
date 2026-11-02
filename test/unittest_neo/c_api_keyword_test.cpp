#include "neo/Jieba.hpp"
#include "neo/KeywordExtractor.hpp"

#include "CApiTestSupport.hpp"
#include "neo_cppjieba.h"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace c_api_test {
namespace {

using TfIdfOwner = std::unique_ptr<neo_jieba_tfidf_v1, decltype(&neo_jieba_v1_tfidf_destroy)>;
using TextRankOwner = std::unique_ptr<neo_jieba_textrank_v1, decltype(&neo_jieba_v1_textrank_destroy)>;
using KeywordsOwner = std::unique_ptr<neo_jieba_keywords_v1, decltype(&neo_jieba_v1_keywords_destroy)>;

constexpr auto idf_path = std::string_view{DICT_DIR "/idf.utf8"};
constexpr auto stop_words_path = std::string_view{DICT_DIR "/stop_words.utf8"};

// Keyword tests share the base fixture's engine and one pair of extractors loaded from the repository's resources.
class CApiKeywordTest : public CApiTest {
protected:
    static void SetUpTestSuite() {
        CApiTest::SetUpTestSuite();
        auto storage = std::array<char, 512>{};
        auto diagnostic = neo_jieba_error_buffer_v1{storage.data(), storage.size(), 0};
        const auto idf = file_source(idf_path);
        const auto stop_words = file_source(stop_words_path);
        neo_jieba_tfidf_v1 *created_tfidf = nullptr;
        ASSERT_EQ(neo_jieba_v1_tfidf_create(&idf, &stop_words, &created_tfidf, &diagnostic), NEO_JIEBA_V1_OK)
            << storage.data();
        tfidf.reset(created_tfidf);
        neo_jieba_textrank_v1 *created_textrank = nullptr;
        ASSERT_EQ(neo_jieba_v1_textrank_create(&stop_words, &created_textrank, &diagnostic), NEO_JIEBA_V1_OK)
            << storage.data();
        textrank.reset(created_textrank);
    }

    static void TearDownTestSuite() {
        textrank.reset();
        tfidf.reset();
        CApiTest::TearDownTestSuite();
    }

    auto make_keywords() -> KeywordsOwner {
        neo_jieba_keywords_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_keywords_create(&created, &error), NEO_JIEBA_V1_OK) << error_message();
        return KeywordsOwner{created, neo_jieba_v1_keywords_destroy};
    }

    auto borrow_keywords(const KeywordsOwner &keywords) -> neo_jieba_borrowed_keywords_v1 {
        auto data = neo_jieba_borrowed_keywords_v1{};
        EXPECT_EQ(neo_jieba_v1_keywords_borrow_data(keywords.get(), &data, &error), NEO_JIEBA_V1_OK);
        return data;
    }

    inline static auto tfidf = TfIdfOwner{nullptr, neo_jieba_v1_tfidf_destroy};
    inline static auto textrank = TextRankOwner{nullptr, neo_jieba_v1_textrank_destroy};
};

// CppJieba's TextRank defaults, an unfiltered TF-IDF request, and CppJieba's keyword demo sentence.
constexpr auto default_textrank_options = neo_jieba_textrank_options_v1{5, 10, 0.85, nullptr, 0};
constexpr auto unfiltered_tfidf_options = neo_jieba_tfidf_options_v1{nullptr, 0};
constexpr auto keyword_text =
    std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};

// One keyword as plain data, comparable between C and C++ results.
struct KeywordSummary {
    std::string word;
    double weight;
    std::string tag;
    std::vector<std::array<uint64_t, 2>> occurrences;

    auto operator==(const KeywordSummary &) const -> bool = default;
};

// Read keywords as a zero-copy caller does: each word is spelled by its first occurrence in the input.
auto summarize(std::string_view input, const neo_jieba_borrowed_keywords_v1 &data) -> std::vector<KeywordSummary> {
    auto result = std::vector<KeywordSummary>{};
    result.reserve(data.count);
    for (const auto &keyword : std::span{data.keywords, static_cast<std::size_t>(data.count)}) {
        auto &summary = result.emplace_back(KeywordSummary{{}, keyword.weight, std::string{tag_text(keyword.tag)}, {}});
        const auto occurrences =
            std::span{data.occurrences + keyword.first_occurrence, static_cast<std::size_t>(keyword.occurrence_count)};
        for (const auto &range : occurrences) {
            summary.occurrences.push_back({range.byte_offset, range.byte_length});
        }
        if (!occurrences.empty()) {
            summary.word = input.substr(occurrences.front().byte_offset, occurrences.front().byte_length);
        }
    }
    return result;
}

// The same summary for a C++ result, the reference for C extraction.
auto summarize(const std::vector<neo_cppjieba::Keyword<char>> &keywords) -> std::vector<KeywordSummary> {
    auto result = std::vector<KeywordSummary>{};
    result.reserve(keywords.size());
    for (const auto &keyword : keywords) {
        auto &summary = result.emplace_back(
            KeywordSummary{keyword.word, keyword.weight, std::string{keyword.tag.to_string_view()}, {}});
        for (const auto &position : keyword.positions) {
            summary.occurrences.push_back(
                {static_cast<uint64_t>(position.source.begin), static_cast<uint64_t>(position.source.size())});
        }
    }
    return result;
}

// The C++ facade C keyword results are checked against, loaded once.
auto reference_jieba() -> const neo_cppjieba::Jieba & {
    static const auto jieba = neo_cppjieba::Jieba{paths.dict_path, paths.model_path, paths.user_dict_path};
    return jieba;
}

// The C++ TF-IDF extractor loaded from the same files as the fixture's C extractor.
auto reference_tfidf() -> const neo_cppjieba::TfIdfExtractor & {
    static const auto extractor = neo_cppjieba::TfIdfExtractor{idf_path, stop_words_path};
    return extractor;
}

// The C++ TextRank extractor loaded from the same stop words as the fixture's C extractor.
auto reference_textrank() -> const neo_cppjieba::TextRankExtractor & {
    static const auto extractor = neo_cppjieba::TextRankExtractor{stop_words_path};
    return extractor;
}

// One worker extracts every sentence with both rankers through its own handles; engine and extractors are shared.
auto extract_all(const neo_jieba_engine_v1 *engine, const neo_jieba_tfidf_v1 *tfidf,
                 const neo_jieba_textrank_v1 *textrank) -> std::vector<std::vector<KeywordSummary>> {
    neo_jieba_workspace_v1 *created_workspace = nullptr;
    neo_jieba_keywords_v1 *created_keywords = nullptr;
    neo_jieba_v1_workspace_create(&created_workspace, nullptr);
    neo_jieba_v1_keywords_create(&created_keywords, nullptr);
    const auto workspace = WorkspaceOwner{created_workspace, neo_jieba_v1_workspace_destroy};
    const auto keywords = KeywordsOwner{created_keywords, neo_jieba_v1_keywords_destroy};
    auto results = std::vector<std::vector<KeywordSummary>>{};
    results.reserve(2 * sentences.size());
    for (const auto sentence : sentences) {
        auto data = neo_jieba_borrowed_keywords_v1{};
        neo_jieba_v1_tfidf_extract_into(tfidf, engine, sentence.data(), sentence.size(), 5, &unfiltered_tfidf_options,
                                        workspace.get(), keywords.get(), nullptr);
        neo_jieba_v1_keywords_borrow_data(keywords.get(), &data, nullptr);
        results.push_back(summarize(sentence, data));
        neo_jieba_v1_textrank_extract_into(textrank, engine, sentence.data(), sentence.size(), 5,
                                           &default_textrank_options, workspace.get(), keywords.get(), nullptr);
        neo_jieba_v1_keywords_borrow_data(keywords.get(), &data, nullptr);
        results.push_back(summarize(sentence, data));
    }
    return results;
}

TEST_F(CApiKeywordTest, TfIdfKeywordsMatchCppExtraction) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), keyword_text.data(), keyword_text.size(), 5,
                                              &unfiltered_tfidf_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK)
        << error_message();
    const auto expected = reference_tfidf().extract(reference_jieba(), keyword_text, 5, neo_cppjieba::TfIdfOptions{});
    ASSERT_EQ(expected.size(), 5U);
    EXPECT_EQ(summarize(keyword_text, borrow_keywords(keywords)), summarize(expected));
}

TEST_F(CApiKeywordTest, TextRankKeywordsMatchCppExtraction) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    ASSERT_EQ(neo_jieba_v1_textrank_extract_into(textrank.get(), engine.get(), keyword_text.data(), keyword_text.size(),
                                                 5, &default_textrank_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK)
        << error_message();
    const auto expected =
        reference_textrank().extract(reference_jieba(), keyword_text, 5, neo_cppjieba::TextRankOptions{});
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(summarize(keyword_text, borrow_keywords(keywords)), summarize(expected));
}

TEST_F(CApiKeywordTest, AllowedTagsFilterBothRankersLikeCpp) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    const auto allowed = std::array{c_tag("ns"), c_tag("n"), c_tag("vn"), c_tag("v")};
    const auto cpp_allowed = std::vector<neo_cppjieba::PosTag>(neo_cppjieba::python_textrank_tags.begin(),
                                                               neo_cppjieba::python_textrank_tags.end());
    const auto tfidf_options = neo_jieba_tfidf_options_v1{allowed.data(), allowed.size()};
    ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), keyword_text.data(), keyword_text.size(), 10,
                                              &tfidf_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto tfidf_result = summarize(keyword_text, borrow_keywords(keywords));
    EXPECT_EQ(tfidf_result, summarize(reference_tfidf().extract(reference_jieba(), keyword_text, 10,
                                                                neo_cppjieba::TfIdfOptions{cpp_allowed})));

    const auto textrank_options = neo_jieba_textrank_options_v1{5, 10, 0.85, allowed.data(), allowed.size()};
    ASSERT_EQ(neo_jieba_v1_textrank_extract_into(textrank.get(), engine.get(), keyword_text.data(), keyword_text.size(),
                                                 10, &textrank_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(summarize(keyword_text, borrow_keywords(keywords)),
              summarize(reference_textrank().extract(reference_jieba(), keyword_text, 10,
                                                     neo_cppjieba::TextRankOptions{5, 10, 0.85, cpp_allowed})));

    ASSERT_FALSE(tfidf_result.empty());
    const auto allowed_text = std::array<std::string_view, 4>{"ns", "n", "vn", "v"};
    for (const auto &keyword : tfidf_result) {
        EXPECT_NE(std::ranges::find(allowed_text, keyword.tag), allowed_text.end()) << keyword.word;
    }
}

TEST_F(CApiKeywordTest, RepeatedKeywordListsEveryOccurrenceInInputOrder) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    const auto input = std::string_view{"我爱苹果，苹果很甜，我每天吃苹果。"};
    ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), input.data(), input.size(), 10,
                                              &unfiltered_tfidf_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto summaries = summarize(input, borrow_keywords(keywords));
    const auto repeated = std::ranges::find(summaries, std::string{"苹果"}, &KeywordSummary::word);
    ASSERT_NE(repeated, summaries.end());
    ASSERT_EQ(repeated->occurrences.size(), 3U);
    EXPECT_TRUE(std::ranges::is_sorted(repeated->occurrences));
    for (const auto &[offset, length] : repeated->occurrences) {
        EXPECT_EQ(input.substr(offset, length), repeated->word);
    }
}

TEST_F(CApiKeywordTest, TopNLimitsTheKeywordCount) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    for (const auto top_n : {uint64_t{0}, uint64_t{2}}) {
        SCOPED_TRACE(top_n);
        ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), keyword_text.data(), keyword_text.size(),
                                                  top_n, &unfiltered_tfidf_options, workspace.get(), keywords.get(),
                                                  &error),
                  NEO_JIEBA_V1_OK);
        EXPECT_EQ(borrow_keywords(keywords).count, top_n);
    }
    ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), keyword_text.data(), keyword_text.size(),
                                              std::numeric_limits<uint64_t>::max(), &unfiltered_tfidf_options,
                                              workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto every = reference_tfidf().extract(reference_jieba(), keyword_text,
                                                 std::numeric_limits<std::size_t>::max(), neo_cppjieba::TfIdfOptions{});
    EXPECT_GT(every.size(), 5U);
    EXPECT_EQ(summarize(keyword_text, borrow_keywords(keywords)), summarize(every));
}

TEST_F(CApiKeywordTest, MemoryKeywordSourcesMatchFiles) {
    const auto idf_text = read_text(idf_path);
    const auto stop_words_text = read_text(stop_words_path);
    const auto idf = memory_source(idf_text);
    const auto stop_words = memory_source(stop_words_text);
    neo_jieba_tfidf_v1 *created = nullptr;
    ASSERT_EQ(neo_jieba_v1_tfidf_create(&idf, &stop_words, &created, &error), NEO_JIEBA_V1_OK) << error_message();
    const auto from_memory = TfIdfOwner{created, neo_jieba_v1_tfidf_destroy};
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    for (const auto *extractor : {from_memory.get(), tfidf.get()}) {
        ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(extractor, engine.get(), keyword_text.data(), keyword_text.size(), 5,
                                                  &unfiltered_tfidf_options, workspace.get(), keywords.get(), &error),
                  NEO_JIEBA_V1_OK);
        EXPECT_EQ(
            summarize(keyword_text, borrow_keywords(keywords)),
            summarize(reference_tfidf().extract(reference_jieba(), keyword_text, 5, neo_cppjieba::TfIdfOptions{})));
    }
}

TEST_F(CApiKeywordTest, KeywordsSurviveTheirExtractorWorkspaceAndEngine) {
    const auto keywords = make_keywords();
    {
        const auto local_engine = make_clone();
        const auto workspace = make_workspace();
        const auto stop_words = file_source(stop_words_path);
        neo_jieba_textrank_v1 *created = nullptr;
        ASSERT_EQ(neo_jieba_v1_textrank_create(&stop_words, &created, &error), NEO_JIEBA_V1_OK);
        const auto extractor = TextRankOwner{created, neo_jieba_v1_textrank_destroy};
        ASSERT_EQ(neo_jieba_v1_textrank_extract_into(extractor.get(), local_engine.get(), keyword_text.data(),
                                                     keyword_text.size(), 5, &default_textrank_options, workspace.get(),
                                                     keywords.get(), &error),
                  NEO_JIEBA_V1_OK);
    }
    const auto expected =
        reference_textrank().extract(reference_jieba(), keyword_text, 5, neo_cppjieba::TextRankOptions{});
    EXPECT_EQ(summarize(keyword_text, borrow_keywords(keywords)), summarize(expected));
}

TEST_F(CApiKeywordTest, FailedExtractionLeavesKeywordsEmpty) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), keyword_text.data(), keyword_text.size(), 5,
                                              &unfiltered_tfidf_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto invalid = std::string_view{"中国\xE4\xB8"};
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), invalid.data(), invalid.size(), 5,
                                              &unfiltered_tfidf_options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_GT(error.message_size, 0U);
    const auto data = borrow_keywords(keywords);
    EXPECT_EQ(data.keywords, nullptr);
    EXPECT_EQ(data.count, 0U);
    EXPECT_EQ(data.occurrences, nullptr);
    EXPECT_EQ(data.occurrence_count, 0U);
}

TEST_F(CApiKeywordTest, KeywordExtractionRejectsMissingHandlesAndInput) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    const auto *text = keyword_text.data();
    const auto size = uint64_t{keyword_text.size()};
    const auto *options = &unfiltered_tfidf_options;
    ASSERT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), text, size, 5, options, workspace.get(),
                                              keywords.get(), &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(nullptr, engine.get(), text, size, 5, options, workspace.get(),
                                              keywords.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(borrow_keywords(keywords).count, 0U);
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), nullptr, text, size, 5, options, workspace.get(),
                                              keywords.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), nullptr, 1, 5, options, workspace.get(),
                                              keywords.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), text, size, 5, options, nullptr,
                                              keywords.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), text, size, 5, options, workspace.get(),
                                              nullptr, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), text, neo_jieba_v1_max_input_bytes() + 1, 5,
                                              options, workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_LIMIT_EXCEEDED);
}

TEST_F(CApiKeywordTest, KeywordOptionsAreValidatedBeforeExtraction) {
    const auto workspace = make_workspace();
    const auto keywords = make_keywords();
    const auto *text = keyword_text.data();
    const auto size = uint64_t{keyword_text.size()};
    auto unpadded = c_tag("n");
    unpadded.data[2] = 'x';
    const auto bad_tags = neo_jieba_tfidf_options_v1{&unpadded, 1};
    const auto missing_tags = neo_jieba_tfidf_options_v1{nullptr, 1};
    for (const auto *options : {&bad_tags, &missing_tags, static_cast<const neo_jieba_tfidf_options_v1 *>(nullptr)}) {
        EXPECT_EQ(neo_jieba_v1_tfidf_extract_into(tfidf.get(), engine.get(), text, size, 5, options, workspace.get(),
                                                  keywords.get(), &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
    }
    for (const auto damping : {-0.1, 1.5, std::numeric_limits<double>::quiet_NaN()}) {
        SCOPED_TRACE(damping);
        const auto options = neo_jieba_textrank_options_v1{5, 10, damping, nullptr, 0};
        EXPECT_EQ(neo_jieba_v1_textrank_extract_into(textrank.get(), engine.get(), text, size, 5, &options,
                                                     workspace.get(), keywords.get(), &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
    }
    const auto unpadded_textrank = neo_jieba_textrank_options_v1{5, 10, 0.85, &unpadded, 1};
    EXPECT_EQ(neo_jieba_v1_textrank_extract_into(textrank.get(), engine.get(), text, size, 5, &unpadded_textrank,
                                                 workspace.get(), keywords.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_textrank_extract_into(textrank.get(), engine.get(), text, size, 5, nullptr, workspace.get(),
                                                 keywords.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
}

TEST_F(CApiKeywordTest, KeywordHandleCreationChecksArgumentsAndSources) {
    const auto idf = file_source(idf_path);
    const auto stop_words = file_source(stop_words_path);
    auto invalid = stop_words;
    invalid.reserved = 1;
    const auto missing = file_source(TEST_DATA_DIR "/c_api_nonexistent_file");
    neo_jieba_tfidf_v1 *tfidf_result = nullptr;
    neo_jieba_textrank_v1 *textrank_result = nullptr;
    EXPECT_EQ(neo_jieba_v1_tfidf_create(&idf, &invalid, &tfidf_result, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_create(nullptr, &stop_words, &tfidf_result, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_create(&idf, &stop_words, nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_textrank_create(&invalid, &textrank_result, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_textrank_create(&stop_words, nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_tfidf_create(&missing, &stop_words, &tfidf_result, &error), NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_NE(error_message().find("c_api_nonexistent_file"), std::string_view::npos) << error_message();
    EXPECT_EQ(neo_jieba_v1_textrank_create(&missing, &textrank_result, &error), NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_EQ(tfidf_result, nullptr);
    EXPECT_EQ(textrank_result, nullptr);

    EXPECT_EQ(neo_jieba_v1_keywords_create(nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    auto data = neo_jieba_borrowed_keywords_v1{nullptr, 3, nullptr, 4};
    EXPECT_EQ(neo_jieba_v1_keywords_borrow_data(nullptr, &data, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(data.count, 0U);
    EXPECT_EQ(data.occurrence_count, 0U);
}

TEST_F(CApiKeywordTest, ConcurrentExtractionWithSeparateHandlesMatchesSerialExtraction) {
    const auto *shared_engine = engine.get();
    const auto *shared_tfidf = tfidf.get();
    const auto *shared_textrank = textrank.get();
    const auto expected = extract_all(shared_engine, shared_tfidf, shared_textrank);
    ASSERT_TRUE(std::ranges::none_of(expected, [](const auto &keywords) { return keywords.empty(); }));

    for (const auto &result : run_workers([shared_engine, shared_tfidf, shared_textrank] {
             return extract_all(shared_engine, shared_tfidf, shared_textrank);
         })) {
        EXPECT_EQ(result, expected);
    }
}

} // namespace
} // namespace c_api_test
