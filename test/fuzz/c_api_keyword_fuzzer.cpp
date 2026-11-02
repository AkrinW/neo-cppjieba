#include "neo/Utf.hpp"

#include "CApiFuzzUtils.hpp"
#include "FuzzUtils.hpp"
#include "neo_cppjieba.h"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

// Tags keywords commonly carry, and one whose padding is not all NUL.
constexpr auto tag_choices = std::array{neo_jieba_pos_tag_v1{{'n'}},      neo_jieba_pos_tag_v1{{'v'}},
                                        neo_jieba_pos_tag_v1{{'n', 's'}}, neo_jieba_pos_tag_v1{{'e', 'n', 'g'}},
                                        neo_jieba_pos_tag_v1{{'x'}},      neo_jieba_pos_tag_v1{{'n', '\0', 'v'}}};

// The documented damping range, its endpoints, and values outside it, including NaN.
constexpr auto damping_choices = std::array{
    0.0, 0.5, 0.85, 1.0, -0.25, 1.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};

// One decoded extraction request; text borrows the fuzz input.
struct KeywordRequest {
    std::uint64_t top_n = 0;
    std::uint64_t window = 0;
    std::uint64_t iterations = 0;
    double damping = 0.0;
    std::vector<neo_jieba_pos_tag_v1> allowed_tags;
    std::string_view text;
};

// Which ranker an extraction uses.
enum class Ranker : std::uint8_t { TfIdf, TextRank };

// Load the repository IDF table and stop words once; extraction only reads the extractors.
auto tfidf() -> const neo_jieba_tfidf_v1 * {
    static const auto *const owned = [] {
        const auto idf = file_source(DICT_DIR "/idf.utf8");
        const auto stop_words = file_source(DICT_DIR "/stop_words.utf8");
        neo_jieba_tfidf_v1 *created = nullptr;
        require(neo_jieba_v1_tfidf_create(&idf, &stop_words, &created, nullptr) == NEO_JIEBA_V1_OK,
                "tfidf_create failed");
        return created;
    }();
    return owned;
}

// Load the repository stop words once for TextRank.
auto textrank() -> const neo_jieba_textrank_v1 * {
    static const auto *const owned = [] {
        const auto stop_words = file_source(DICT_DIR "/stop_words.utf8");
        neo_jieba_textrank_v1 *created = nullptr;
        require(neo_jieba_v1_textrank_create(&stop_words, &created, nullptr) == NEO_JIEBA_V1_OK,
                "textrank_create failed");
        return created;
    }();
    return owned;
}

// Bytes in order: top_n, window, iterations, damping, the allowed tag count and that many tag bytes; then the text.
auto decode_request(ByteReader &reader) -> KeywordRequest {
    auto request = KeywordRequest{};
    request.top_n = reader.byte() % 24;
    request.window = reader.byte() % 8;
    request.iterations = reader.byte() % 16;
    request.damping = damping_choices[reader.byte() % damping_choices.size()];
    const auto tag_count = reader.byte() % 4;
    for (auto index = 0; index < tag_count; ++index) {
        request.allowed_tags.push_back(tag_choices[reader.byte() % tag_choices.size()]);
    }
    request.text = reader.rest();
    return request;
}

// Option errors come before decoding errors, matching the order the adapter checks them in.
auto expected_status(Ranker ranker, const KeywordRequest &request) -> neo_jieba_status_v1 {
    const auto damping_in_range = request.damping >= 0.0 && request.damping <= 1.0;
    if (!std::ranges::all_of(request.allowed_tags, is_padded_tag)
        || (ranker == Ranker::TextRank && !damping_in_range)) {
        return NEO_JIEBA_V1_INVALID_ARGUMENT;
    }
    return Utf::is_valid(request.text) ? NEO_JIEBA_V1_OK : NEO_JIEBA_V1_OPERATION_FAILED;
}

// Run one extraction into the shared workspace and keywords result.
auto extract(Ranker ranker, const KeywordRequest &request, neo_jieba_workspace_v1 *workspace,
             neo_jieba_keywords_v1 *keywords, ErrorProbe &error) -> neo_jieba_status_v1 {
    const auto *tags = request.allowed_tags.empty() ? nullptr : request.allowed_tags.data();
    const auto &text = request.text;
    if (ranker == Ranker::TfIdf) {
        const auto options = neo_jieba_tfidf_options_v1{tags, request.allowed_tags.size()};
        return neo_jieba_v1_tfidf_extract_into(tfidf(), repository_engine(), text.data(), text.size(), request.top_n,
                                               &options, workspace, keywords, error.buffer());
    }
    const auto options = neo_jieba_textrank_options_v1{request.window, request.iterations, request.damping, tags,
                                                       request.allowed_tags.size()};
    return neo_jieba_v1_textrank_extract_into(textrank(), repository_engine(), text.data(), text.size(), request.top_n,
                                              &options, workspace, keywords, error.buffer());
}

// Slice the input by a borrowed range, which must lie inside it and be nonempty.
auto slice(std::string_view input, const neo_jieba_token_range_v1 &range) -> std::string_view {
    require(range.byte_length > 0 && range.byte_offset <= input.size()
                && range.byte_length <= input.size() - range.byte_offset,
            "an occurrence range exceeds the input");
    return input.substr(range.byte_offset, range.byte_length);
}

// A keyword's occurrences are its own, in input order, and all spell the word, which is tagged as lookup_tag does.
auto check_keyword(const neo_jieba_keyword_v1 &keyword, std::span<const neo_jieba_token_range_v1> occurrences,
                   std::vector<bool> &claimed, const KeywordRequest &request) -> std::string_view {
    require(keyword.reserved == 0 && is_padded_tag(keyword.tag), "a keyword has a reserved field or unpadded tag");
    require(std::isfinite(keyword.weight), "a keyword weight is not finite");
    require(keyword.occurrence_count > 0 && keyword.first_occurrence <= occurrences.size()
                && keyword.occurrence_count <= occurrences.size() - keyword.first_occurrence,
            "a keyword's occurrences exceed the occurrence array");
    const auto first = static_cast<std::size_t>(keyword.first_occurrence);
    const auto own = occurrences.subspan(first, static_cast<std::size_t>(keyword.occurrence_count));
    const auto word = slice(request.text, own.front());
    for (std::size_t index = 0; index < own.size(); ++index) {
        require(!claimed[first + index], "two keywords share an occurrence");
        claimed[first + index] = true;
        require(slice(request.text, own[index]) == word, "an occurrence spells another word");
        require(index == 0 || own[index - 1].byte_offset < own[index].byte_offset, "occurrences are out of order");
    }

    auto tag = neo_jieba_pos_tag_v1{};
    require(neo_jieba_v1_lookup_tag(repository_engine(), word.data(), word.size(), &tag, nullptr) == NEO_JIEBA_V1_OK,
            "lookup_tag rejected a keyword");
    require(same_tag(tag, keyword.tag), "a keyword tag differs from lookup_tag");
    require(request.allowed_tags.empty()
                || std::ranges::any_of(request.allowed_tags,
                                       [&](const neo_jieba_pos_tag_v1 &allowed) { return same_tag(allowed, tag); }),
            "a keyword tag is not allowed");
    return word;
}

// A filled result holds at most top_n distinct keywords, from the highest weight, ties in first-occurrence order.
auto check_keywords(const neo_jieba_keywords_v1 *keywords, const KeywordRequest &request) -> void {
    auto data = neo_jieba_borrowed_keywords_v1{};
    require(neo_jieba_v1_keywords_borrow_data(keywords, &data, nullptr) == NEO_JIEBA_V1_OK,
            "keywords_borrow_data failed");
    const auto found = borrowed(data.keywords, data.count);
    const auto occurrences = borrowed(data.occurrences, data.occurrence_count);
    require(found.size() <= request.top_n, "more keywords than top_n");

    auto claimed = std::vector<bool>(occurrences.size(), false);
    auto words = std::vector<std::string_view>{};
    words.reserve(found.size());
    for (const auto &keyword : found) {
        words.push_back(check_keyword(keyword, occurrences, claimed, request));
    }
    std::ranges::sort(words);
    require(std::ranges::adjacent_find(words) == words.end(), "a word appears as two keywords");

    const auto ordered = [&](const neo_jieba_keyword_v1 &left, const neo_jieba_keyword_v1 &right) {
        const auto left_offset = occurrences[static_cast<std::size_t>(left.first_occurrence)].byte_offset;
        const auto right_offset = occurrences[static_cast<std::size_t>(right.first_occurrence)].byte_offset;
        return left.weight > right.weight || (left.weight == right.weight && left_offset < right_offset);
    };
    require(std::ranges::adjacent_find(found, std::not_fn(ordered)) == found.end(), "keywords are out of order");
}

// Each extraction returns its documented status; failures leave the result empty.
auto check_extraction(Ranker ranker, const KeywordRequest &request, neo_jieba_workspace_v1 *workspace,
                      neo_jieba_keywords_v1 *keywords, ErrorProbe &error) -> void {
    const auto status = extract(ranker, request, workspace, keywords, error);
    error.check(status);
    require(status == expected_status(ranker, request), "an extraction returned an unexpected status");
    if (status == NEO_JIEBA_V1_OK) {
        check_keywords(keywords, request);
        return;
    }
    auto data = neo_jieba_borrowed_keywords_v1{};
    require(neo_jieba_v1_keywords_borrow_data(keywords, &data, nullptr) == NEO_JIEBA_V1_OK && data.count == 0
                && data.occurrence_count == 0,
            "a failed extraction left keywords");
}

} // namespace

// The first byte orders the rankers and picks the diagnostic capacity; the request follows. Both rankers fill the
// same workspace and keywords result, so the second extraction reuses storage the first one grew.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    const auto flags = reader.byte();
    const auto request = decode_request(reader);
    auto error = ErrorProbe{static_cast<std::uint8_t>(flags >> 1)};

    neo_jieba_keywords_v1 *created = nullptr;
    require(neo_jieba_v1_keywords_create(&created, nullptr) == NEO_JIEBA_V1_OK, "keywords_create failed");
    const auto keywords = KeywordsOwner{created, neo_jieba_v1_keywords_destroy};
    const auto workspace = make_workspace();

    const auto first = (flags & 1) != 0 ? Ranker::TextRank : Ranker::TfIdf;
    const auto second = first == Ranker::TfIdf ? Ranker::TextRank : Ranker::TfIdf;
    check_extraction(first, request, workspace.get(), keywords.get(), error);
    check_extraction(second, request, workspace.get(), keywords.get(), error);
    return 0;
}
