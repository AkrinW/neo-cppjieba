#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/MPSegment.hpp"
#include "neo/algorithm/MixSegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <cassert>
#include <limits>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

/// Stateless query-mode segmentation operator.
///
/// Query mode ("搜索引擎模式") is designed for search-engine indexing. It follows
/// the MIX route, and before each word longer than 2 characters it additionally emits
/// all 2-gram and then 3-gram sub-words that exist in the dictionary. This produces
/// more fine-grained tokens that improve search recall.
///
///   Example: "中国科学院" → "中国/科学/学院/科学院/中国科学院"
///
/// Sub-word membership is recorded while the route is built, so no DAG is stored.
template <bool hmm = true>
struct QuerySegment {
    // Replace output while reusing all algorithm buffers.
    static auto cut_into(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes,
                         std::vector<WordRange> &result, detail::SegmentScratch &scratch) -> void {
        assert_check([&] { return runes.size() <= std::numeric_limits<RuneIndex>::max(); },
                     "QuerySegment: input exceeds the word-range limit");
        result.clear();
        result.reserve(runes.size());
        cut_append(dict, model, runes, 0, result, scratch);
    }

private:
    /// Append query-mode segmentation results while preserving separator runes as standalone tokens.
    static auto cut_append(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes, RuneIndex pos,
                           std::vector<WordRange> &result, detail::SegmentScratch &scratch) -> void {
        assert_check(
            [&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
            "QuerySegment: global word offsets overflow");
        detail::for_each_separated(
            runes,
            [&](std::span<const Rune> segment, RuneIndex offset) {
                cut_one_segment(dict, model, segment, static_cast<RuneIndex>(pos + offset), result, scratch);
            },
            [&](RuneIndex separator) {
                result.push_back(
                    {static_cast<RuneIndex>(pos + separator), static_cast<RuneIndex>(pos + separator + 1)});
            },
            scratch.separators);
    }

    /// Check whether the dictionary contains the two- or three-rune word [begin, end) of the segment.
    [[nodiscard]]
    static auto has_short_match(std::span<const detail::SearchSubwords> matches, RuneIndex begin,
                                RuneIndex end) noexcept -> bool {
        assert(begin < end && end <= matches.size());
        const auto length = end - begin;
        assert(length == 2 || length == 3);
        return length == 2 ? matches[to_index(begin)].bigram : matches[to_index(begin)].trigram;
    }

    /// Emit the 2-gram and 3-gram dictionary sub-words of a route word from the recorded membership flags.
    static auto append_sub_words(std::span<const detail::SearchSubwords> matches, RuneIndex segment_offset,
                                 WordRange word, std::vector<WordRange> &result) -> void {
        assert_check(
            [&] {
                return matches.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - segment_offset);
            },
            "QuerySegment: global word offsets overflow");
        assert_check([&] { return segment_offset <= word.begin && word.begin < word.end; },
                     "QuerySegment: invalid global word range");
        assert_check([&] { return static_cast<RuneIndex>(word.end - segment_offset) <= matches.size(); },
                     "QuerySegment: word range exceeds the match flags");
        auto len = word.size();
        auto local_begin = word.begin - segment_offset;

        if (len > 2) {
            for (auto i = RuneIndex{0}; i <= len - 2; ++i) {
                auto begin = local_begin + i;
                auto end = begin + 2;
                if (has_short_match(matches, static_cast<RuneIndex>(begin), static_cast<RuneIndex>(end))) {
                    result.push_back(
                        WordRange{static_cast<RuneIndex>(word.begin + i), static_cast<RuneIndex>(word.begin + i + 2)});
                }
            }
        }

        if (len > 3) {
            for (auto i = RuneIndex{0}; i <= len - 3; ++i) {
                auto begin = local_begin + i;
                auto end = begin + 3;
                if (has_short_match(matches, static_cast<RuneIndex>(begin), static_cast<RuneIndex>(end))) {
                    result.push_back(
                        WordRange{static_cast<RuneIndex>(word.begin + i), static_cast<RuneIndex>(word.begin + i + 3)});
                }
            }
        }
    }

    /// Emit query-mode tokens for a segment-local word, including HMM words off the MP route:
    /// every start position retains short-word flags, independent of the chosen route.
    static auto append_query_local_word(std::span<const detail::SearchSubwords> matches, WordRange word,
                                        RuneIndex segment_offset, std::vector<WordRange> &result) -> void {
        assert_check(
            [&] {
                return matches.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - segment_offset);
            },
            "QuerySegment: global word offsets overflow");
        assert_check([&] { return word.begin < word.end && word.end <= matches.size(); },
                     "QuerySegment: invalid local HMM word range");
        append_query_word(matches, segment_offset,
                          WordRange{static_cast<RuneIndex>(segment_offset + word.begin),
                                    static_cast<RuneIndex>(segment_offset + word.end)},
                          result);
    }

    /// Emit the searchable sub-words of a global word, then the word itself.
    static auto append_query_word(std::span<const detail::SearchSubwords> matches, RuneIndex segment_offset,
                                  WordRange word, std::vector<WordRange> &result) -> void {
        append_sub_words(matches, segment_offset, word, result);
        result.push_back(word);
    }

    /// Build the route with short-word flags, then emit final words without intermediate word arrays.
    static auto cut_one_segment(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes, RuneIndex pos,
                                std::vector<WordRange> &result, detail::SegmentScratch &scratch) -> void {
        assert_check(
            [&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
            "QuerySegment: global word offsets overflow");
        if (runes.empty()) {
            return;
        }

        detail::mp_build_route_from_matches<detail::MPMatchMode::Search>(dict, runes, scratch);
        assert(scratch.route.size() >= runes.size() && scratch.search_subwords.size() >= runes.size());
        const auto matches = std::span<const detail::SearchSubwords>{scratch.search_subwords}.first(runes.size());
        detail::mix_emit_route<hmm>(
            dict, model, runes, [&](WordRange word) { append_query_local_word(matches, word, pos, result); }, scratch);
    }
};

static_assert(SegmentPolicy<QuerySegment<true>> && SegmentPolicy<QuerySegment<false>>);

} // namespace neo_cppjieba::detail
