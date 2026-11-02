#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

/// Select route construction alone or also retain short-word matches for SEARCH.
enum class MPMatchMode : uint8_t { PathOnly, Search };

/// Build the maximum-probability route of an n-rune segment into scratch.route by reverse DP.
/// for_each_edge(begin, emit) calls emit(end, weight) for every candidate word [begin, end);
/// kMissingWordWeight marks an unknown single rune, which scores with the dictionary fallback.
template <RouteEdges ForEachEdge>
inline auto mp_build_route(const DictTrie &dict, size_t n, const ForEachEdge &for_each_edge, SegmentScratch &scratch)
    -> void {
    assert(size_fits_capacity<RuneIndex>(n));
    if (n == 0) {
        return;
    }

    // DP node: cumulative best weight from position i to end, and the next_pos chosen by that optimal edge.
    auto &dp = scratch.route;
    // Only the first n rows are active; retaining the high-water size avoids reinitializing grown spans.
    if (dp.size() < n) {
        dp.resize(n);
    }

    // Unknown runes score with the dictionary minimum weight.
    auto fallback = dict.min_weight();
    assert_check([&] { return std::isfinite(fallback) && fallback <= 0.0; },
                 "MPSegment: invalid dictionary minimum weight");
    if (fallback == 0.0) {
        // A single-entry dictionary still needs a penalty for unknown runes: one pseudo-count out of sum + 1.
        const auto freq_sum = dict.freq_sum();
        assert_check([&] { return std::isfinite(freq_sum) && freq_sum > 0.0; },
                     "MPSegment: invalid dictionary frequency sum");
        fallback = static_cast<WordWeight>(-std::log1p(static_cast<double>(freq_sum)));
    }

    for (auto i = n; i > 0;) {
        --i;
        dp[i] = MPNode{-std::numeric_limits<WordWeight>::infinity(), 0};
        auto best_is_dictionary_word = false;

        for_each_edge(static_cast<RuneIndex>(i), [&](RuneIndex next_pos, WordWeight word_weight) {
            assert_check([&] { return i < next_pos && next_pos <= n; },
                         "MPSegment: route edge must advance within the rune span");
            // A real zero weight participates in scoring as-is; only the missing sentinel uses the fallback.
            const auto is_dictionary_word = word_weight != kMissingWordWeight;
            const auto weight = is_dictionary_word ? word_weight : fallback;
            assert_check([&] { return std::isfinite(weight); }, "MPSegment: non-finite edge weight");

            auto total = weight;
            if (next_pos < n) {
                total += dp[to_index(next_pos)].weight;
            }
            assert_check([&] { return std::isfinite(total); }, "MPSegment: non-finite accumulated weight");

            // On equal totals a dictionary word beats the unknown-rune fallback.
            if (total > dp[i].weight || (total == dp[i].weight && is_dictionary_word && !best_is_dictionary_word)) {
                dp[i].weight = total;
                dp[i].next_pos = next_pos;
                best_is_dictionary_word = is_dictionary_word;
            }
        });
    }
}

/// Build the MP route of a separator-free segment directly from trie matches, without materializing a DAG.
/// Search mode also records two- and three-rune dictionary membership at every start position.
template <MPMatchMode mode>
inline auto mp_build_route_from_matches(const DictTrie &dict, std::span<const Rune> runes, SegmentScratch &scratch)
    -> void {
    if constexpr (mode == MPMatchMode::Search) {
        if (scratch.search_subwords.size() < runes.size()) {
            scratch.search_subwords.resize(runes.size());
        }
    }
    const auto for_each_edge = [&](RuneIndex begin, const auto &emit) {
        if constexpr (mode == MPMatchMode::Search) {
            scratch.search_subwords[to_index(begin)] = {};
        }
        const auto single_end = static_cast<RuneIndex>(begin + 1);
        auto has_single = false;
        dict.for_each_match_from(runes, begin, [&](RuneIndex end, const DictUnit &word) {
            has_single |= end == single_end;
            if constexpr (mode == MPMatchMode::Search) {
                auto &subwords = scratch.search_subwords[to_index(begin)];
                subwords.bigram |= end - begin == 2;
                subwords.trigram |= end - begin == 3;
            }
            emit(end, word.weight);
        });
        // A known single rune replaces the fallback. Otherwise score the fallback last;
        // the shared tie rule still prefers a dictionary word with the same total weight.
        if (!has_single) {
            emit(single_end, kMissingWordWeight);
        }
    };
    mp_build_route(dict, runes.size(), for_each_edge, scratch);
}

/// Visit the route built for an n-rune segment as local word ranges, in input order.
template <WordEmitter Emit>
inline auto mp_emit_route(size_t n, const Emit &emit, const SegmentScratch &scratch) -> void {
    assert(scratch.route.size() >= n);
    const auto &route = scratch.route;
    for (auto begin = RuneIndex{0}; begin < n;) {
        const auto end = route[to_index(begin)].next_pos;
        assert_check([&] { return begin < end && end <= n; }, "MPSegment: route must advance within the rune span");
        emit(WordRange{begin, end});
        begin = end;
    }
}

/// Append MP segmentation results at global offset pos, keeping separator runes as standalone tokens.
inline auto mp_cut_append(const DictTrie &dict, std::span<const Rune> runes, RuneIndex pos, std::vector<WordRange> &out,
                          SegmentScratch &scratch) -> void {
    assert_check([&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
                 "MPSegment: global word offsets overflow");
    const auto appended_begin = out.size();
    for_each_separated(
        runes,
        [&](std::span<const Rune> segment, RuneIndex offset) {
            if (segment.empty()) {
                return;
            }
            const auto base = static_cast<RuneIndex>(pos + offset);
            mp_build_route_from_matches<MPMatchMode::PathOnly>(dict, segment, scratch);
            mp_emit_route(
                segment.size(),
                [&](WordRange word) {
                    out.push_back({static_cast<RuneIndex>(base + word.begin), static_cast<RuneIndex>(base + word.end)});
                },
                scratch);
        },
        [&](RuneIndex separator) {
            out.push_back({static_cast<RuneIndex>(pos + separator), static_cast<RuneIndex>(pos + separator + 1)});
        },
        scratch.separators);
    assert_check(
        [&] {
            return valid_segment_partition_at(std::span<const WordRange>{out}.subspan(appended_begin), runes.size(),
                                              pos);
        },
        "MPSegment: words must cover the rune span exactly once");
}

/// Stateless maximum-probability segmentation operator.
///
/// MP mode ("最大概率模式") uses dynamic programming over dictionary matches
/// to find the segmentation path with the highest total log-probability.
/// The input is split by default separators first; each separator is emitted as its own token.
struct MPSegment {
    // Replace output while reusing scratch across every separator-free segment.
    static auto cut_into(const DictTrie &dict, const HMModel &, std::span<const Rune> runes,
                         std::vector<WordRange> &range, detail::SegmentScratch &scratch) -> void {
        assert_check([&] { return detail::size_fits_capacity<RuneIndex>(runes.size()); },
                     "MPSegment: input exceeds the word-range limit");
        range.clear();
        range.reserve(runes.size() / 2);
        detail::mp_cut_append(dict, runes, 0, range, scratch);
    }
};

static_assert(SegmentPolicy<MPSegment>);

} // namespace neo_cppjieba::detail
