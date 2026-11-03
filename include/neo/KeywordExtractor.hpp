#pragma once

#include "neo/Fwd.hpp"   // IWYU pragma: export
#include "neo/Jieba.hpp" // IWYU pragma: export
#include "neo/algorithm/KeywordRank.hpp"
#include "neo/concept/Text.hpp" // IWYU pragma: export
#include "neo/options/CutMode.hpp"
#include "neo/options/KeywordOptions.hpp" // IWYU pragma: export
#include "neo/resource/KeywordResource.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/DictSource.hpp" // IWYU pragma: export
#include "neo/value/Keyword.hpp"    // IWYU pragma: export
#include "neo/value/PosTag.hpp"     // IWYU pragma: export
#include "neo/value/Rune.hpp"       // IWYU pragma: export
#include "neo/workspace/KeywordState.hpp"
#include "neo/workspace/KeywordWorkspace.hpp" // IWYU pragma: export

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace neo_cppjieba {

namespace detail {

// Segment once with CutMode::MIX, then keep candidate words after stop-word and tag filtering.
template <CharType CharT>
auto segment_candidates(const Jieba &jieba, const StopWords &stop_words, std::basic_string_view<CharT> source,
                        std::span<const PosTag> allowed_tags, KeywordState &state) -> void {
    detail::decode_with_offset_into(source, state.decoded);
    jieba.cut_runes_into(state.decoded.runes, CutMode::MIX, state.ranges, state.segment);
    const auto tag_of = [&](std::span<const Rune> word) {
        return jieba.lookup_tag(word);
    };
    collect_candidates(stop_words, tag_of, state.decoded, state.ranges, allowed_tags, state.table);
}

} // namespace detail

// TF-IDF keywords, matching CppJieba's KeywordExtractor: segment with CutMode::MIX, drop one-rune words,
// stop words and words outside options.allowed_tags, then weight each distinct word by occurrence count times IDF.
// The extractor owns only its keyword resources. Each call takes the Jieba to segment with, so any instance,
// including a clone() with runtime edits, can be used; extraction is const and shares no mutable state.
// Options are configuration of one call: they are read during it and never stored.
class TfIdfExtractor {
public:
    TfIdfExtractor(std::string_view idf_path, std::string_view stop_words_path)
        : TfIdfExtractor(DictSource::file(idf_path), DictSource::file(stop_words_path)) {
    }

    TfIdfExtractor(const DictSource &idf, const DictSource &stop_words) : idf_(idf), stop_words_(stop_words) {
    }

    TfIdfExtractor(TfIdfExtractor &&) noexcept = default;
    auto operator=(TfIdfExtractor &&) noexcept -> TfIdfExtractor & = default;
    auto operator=(const TfIdfExtractor &) -> TfIdfExtractor & = delete;

    // Copying the IDF table and stop words stays explicit at call sites.
    [[nodiscard]]
    auto clone() const -> TfIdfExtractor {
        return TfIdfExtractor{*this};
    }

    template <StringLike Input>
    [[nodiscard]]
    auto extract(const Jieba &jieba, const Input &input, size_t top_n, const TfIdfOptions &options) const
        -> std::vector<Keyword<output_char_type_t<Input>>> {
        auto workspace = KeywordWorkspace{};
        auto result = std::vector<Keyword<output_char_type_t<Input>>>{};
        extract_into(jieba, input, top_n, options, result, workspace);
        return result;
    }

    // Replaces out, reusing its elements' capacity. Input must not alias out.
    template <StringLike Input>
    auto extract_into(const Jieba &jieba, const Input &input, size_t top_n, const TfIdfOptions &options,
                      std::vector<Keyword<output_char_type_t<Input>>> &out, KeywordWorkspace &workspace) const -> void {
        const auto source = detail::as_view(input);
        auto &state = detail::keyword_state(workspace);
        detail::segment_candidates(jieba, stop_words_, source, options.allowed_tags, state);
        detail::weigh_tfidf(idf_, state.decoded.runes, state.table);
        detail::select_keywords(source, state.table, top_n, out, state.selection);
    }

    // Words missing from the IDF file use the average of its values.
    [[nodiscard]]
    auto idf_average() const noexcept -> double {
        return idf_.average();
    }

private:
    TfIdfExtractor(const TfIdfExtractor &other) : idf_(other.idf_.clone()), stop_words_(other.stop_words_.clone()) {
    }

    detail::IdfTable idf_;
    detail::StopWords stop_words_;
};

// TextRank keywords, matching CppJieba's TextRankExtractor: candidates are filtered as in TfIdfExtractor,
// co-occurrence within the window adds undirected edges, and scores are normalized so the best is 1.
// The extractor owns only its keyword resources. Each call takes the Jieba to segment with, so any instance,
// including a clone() with runtime edits, can be used; extraction is const and shares no mutable state.
// Options are configuration of one call, checked at its entry and never stored.
class TextRankExtractor {
public:
    explicit TextRankExtractor(std::string_view stop_words_path)
        : TextRankExtractor(DictSource::file(stop_words_path)) {
    }

    explicit TextRankExtractor(const DictSource &stop_words) : stop_words_(stop_words) {
    }

    TextRankExtractor(TextRankExtractor &&) noexcept = default;
    auto operator=(TextRankExtractor &&) noexcept -> TextRankExtractor & = default;
    auto operator=(const TextRankExtractor &) -> TextRankExtractor & = delete;

    // Copying the stop words stays explicit at call sites.
    [[nodiscard]]
    auto clone() const -> TextRankExtractor {
        return TextRankExtractor{*this};
    }

    template <StringLike Input>
    [[nodiscard]]
    auto extract(const Jieba &jieba, const Input &input, size_t top_n, const TextRankOptions &options) const
        -> std::vector<Keyword<output_char_type_t<Input>>> {
        auto workspace = KeywordWorkspace{};
        auto result = std::vector<Keyword<output_char_type_t<Input>>>{};
        extract_into(jieba, input, top_n, options, result, workspace);
        return result;
    }

    // Replaces out, reusing its elements' capacity. Input must not alias out.
    template <StringLike Input>
    auto extract_into(const Jieba &jieba, const Input &input, size_t top_n, const TextRankOptions &options,
                      std::vector<Keyword<output_char_type_t<Input>>> &out, KeywordWorkspace &workspace) const -> void {
        detail::check_text_rank_options(options);
        const auto source = detail::as_view(input);
        auto &state = detail::keyword_state(workspace);
        detail::segment_candidates(jieba, stop_words_, source, options.allowed_tags, state);
        detail::weigh_text_rank(state.decoded.runes, options, state.table, state.text_rank);
        detail::select_keywords(source, state.table, top_n, out, state.selection);
    }

private:
    TextRankExtractor(const TextRankExtractor &other) : stop_words_(other.stop_words_.clone()) {
    }

    detail::StopWords stop_words_;
};

} // namespace neo_cppjieba
