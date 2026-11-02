#pragma once

#include "neo/algorithm/Output.hpp"
#include "neo/concept/Keyword.hpp"
#include "neo/concept/Text.hpp"
#include "neo/options/KeywordOptions.hpp"
#include "neo/resource/KeywordResource.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/RuneString.hpp"
#include "neo/value/Keyword.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/TokenView.hpp"
#include "neo/workspace/KeywordState.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace neo_cppjieba::detail {

// Keep the segmented words that pass the one-rune, stop-word and tag filters.
// A word's tag is looked up once, when it first passes the other filters.
template <TagLookup TagOf>
auto collect_candidates(const StopWords &stop_words, const TagOf &tag_of, const UnicodeWithOffset &text,
                        std::span<const WordRange> words, std::span<const PosTag> allowed_tags, CandidateTable &table)
    -> void {
    table.index.clear();
    table.candidates.clear();
    table.occurrences.clear();
    for (const auto range : words) {
        const auto runes = range.slice(text.runes);
        const auto word = rune_view(runes);
        if (word.size() == 1 || stop_words.contains(word)) {
            continue;
        }
        const auto position = TokenPosition{range, to_source_range(range, text.offsets)};
        auto it = table.index.find(word);
        if (it == table.index.end()) {
            const auto tag = tag_of(runes);
            if (!allowed_tags.empty() && std::ranges::find(allowed_tags, tag) == allowed_tags.end()) {
                continue;
            }
            it = table.index.emplace(word, table.candidates.size()).first;
            table.candidates.push_back({position, 0.0, tag});
        }
        table.occurrences.push_back({it->second, position});
    }
}

// TF-IDF: each distinct word weighs its occurrence count times its IDF.
inline auto weigh_tfidf(const IdfTable &idf, std::span<const Rune> runes, CandidateTable &table) -> void {
    for (const auto &occurrence : table.occurrences) {
        table.candidates[occurrence.candidate].weight += 1.0;
    }
    for (auto &candidate : table.candidates) {
        candidate.weight *= idf.idf(candidate.word(runes));
    }
}

// Rank candidates by code point order of their words into scratch.ranks; scratch.by_word holds the inverse.
inline auto rank_by_word(std::span<const Rune> runes, const CandidateTable &table, TextRankScratch &scratch) -> void {
    const auto candidate_count = table.candidates.size();
    scratch.by_word.resize(candidate_count);
    std::iota(scratch.by_word.begin(), scratch.by_word.end(), size_t{0});
    std::ranges::sort(scratch.by_word, {}, [&](size_t id) { return table.candidates[id].word(runes); });
    scratch.ranks.resize(candidate_count);
    for (auto rank = size_t{0}; rank < candidate_count; ++rank) {
        scratch.ranks[scratch.by_word[rank]] = rank;
    }
}

// Build one merged adjacency array per node, neighbors in rank order; returns the number of nodes with edges.
// Each pair within the window adds weight 1 in both directions; a word next to itself gets a self-loop of weight 2.
inline auto build_cooccurrence_graph(const CandidateTable &table, size_t window, TextRankScratch &scratch) -> size_t {
    auto &edges = scratch.edges;
    edges.clear();
    const auto &occurrences = table.occurrences;
    for (auto i = size_t{0}; i < occurrences.size(); ++i) {
        const auto from = scratch.ranks[occurrences[i].candidate];
        for (auto j = i + 1; j < occurrences.size() && j - i < window; ++j) {
            const auto to = scratch.ranks[occurrences[j].candidate];
            edges.push_back({from, to, 1.0});
            edges.push_back({to, from, 1.0});
        }
    }

    // Merge parallel edges into one adjacency array per node, neighbors in rank order.
    std::ranges::sort(edges, {}, [](const KeywordEdge &edge) { return std::pair{edge.from, edge.to}; });
    auto merged = size_t{0};
    for (const auto &edge : edges) {
        if (merged > 0 && edges[merged - 1].from == edge.from && edges[merged - 1].to == edge.to) {
            edges[merged - 1].weight += edge.weight;
        } else {
            edges[merged++] = edge;
        }
    }
    edges.resize(merged);
    const auto candidate_count = table.candidates.size();
    scratch.edge_begin.assign(candidate_count + 1, 0);
    for (const auto &edge : edges) {
        ++scratch.edge_begin[edge.from + 1];
    }
    auto node_count = size_t{0};
    for (auto rank = size_t{0}; rank < candidate_count; ++rank) {
        node_count += scratch.edge_begin[rank + 1] > 0 ? size_t{1} : size_t{0};
        scratch.edge_begin[rank + 1] += scratch.edge_begin[rank];
    }
    return node_count;
}

// Run TextRank over the graph, then normalize so the best score is 1 into candidate weights.
// Words without edges keep score 0 and still take part in normalization, as in CppJieba.
inline auto iterate_text_rank(size_t node_count, size_t iterations, double damping, CandidateTable &table,
                              TextRankScratch &scratch) -> void {
    assert_check([&] { return node_count > 0; }, "TextRank: iteration needs at least one connected node");
    const auto candidate_count = table.candidates.size();
    const auto neighbors = [&](size_t rank) {
        return std::span{scratch.edges}.subspan(scratch.edge_begin[rank],
                                                scratch.edge_begin[rank + 1] - scratch.edge_begin[rank]);
    };
    auto &scores = scratch.scores;
    auto &out_sums = scratch.out_sums;
    scores.assign(candidate_count, 0.0);
    out_sums.assign(candidate_count, 0.0);
    const auto initial = 1.0 / static_cast<double>(node_count);
    for (auto rank = size_t{0}; rank < candidate_count; ++rank) {
        for (const auto &edge : neighbors(rank)) {
            scores[rank] = initial;
            out_sums[rank] += edge.weight;
        }
    }
    for (auto iteration = size_t{0}; iteration < iterations; ++iteration) {
        for (auto rank = size_t{0}; rank < candidate_count; ++rank) {
            const auto adjacent = neighbors(rank);
            if (adjacent.empty()) {
                continue;
            }
            auto sum = 0.0;
            for (const auto &edge : adjacent) {
                sum += edge.weight / out_sums[edge.to] * scores[edge.to];
            }
            scores[rank] = (1 - damping) + damping * sum;
        }
    }

    const auto [min_it, max_it] = std::ranges::minmax_element(scores);
    const auto floor = *min_it / 10.0;
    for (auto rank = size_t{0}; rank < candidate_count; ++rank) {
        table.candidates[scratch.by_word[rank]].weight = (scores[rank] - floor) / (*max_it - floor);
    }
}

// CppJieba updates scores in place, visiting nodes and neighbors in UTF-8 byte order of the words.
// Ranks sort words by code point, which is the same order, so every floating-point step matches.
inline auto weigh_text_rank(std::span<const Rune> runes, const TextRankOptions &options, CandidateTable &table,
                            TextRankScratch &scratch) -> void {
    rank_by_word(runes, table, scratch);
    const auto node_count = build_cooccurrence_graph(table, options.window, scratch);
    if (node_count == 0) {
        return;
    }
    iterate_text_rank(node_count, options.iterations, options.damping, table, scratch);
}

// Keep the top_n highest weights; equal weights keep first-occurrence order.
// Output elements are overwritten in place so their strings and position arrays keep their capacity.
template <CharType CharT>
auto select_keywords(std::basic_string_view<CharT> source, const CandidateTable &table, size_t top_n,
                     std::vector<Keyword<CharT>> &out, SelectionScratch &scratch) -> void {
    const auto &candidates = table.candidates;
    auto &order = scratch.order;
    order.resize(candidates.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto count = std::min(top_n, order.size());
    const auto ranked = [&](size_t lhs, size_t rhs) {
        const auto lhs_weight = candidates[lhs].weight;
        const auto rhs_weight = candidates[rhs].weight;
        return lhs_weight != rhs_weight ? lhs_weight > rhs_weight : lhs < rhs;
    };
    std::ranges::partial_sort(order, order.begin() + static_cast<std::ptrdiff_t>(count), ranked);

    constexpr auto unselected = std::numeric_limits<size_t>::max();
    scratch.slots.assign(candidates.size(), unselected);
    out.resize(count);
    for (auto slot = size_t{0}; slot < count; ++slot) {
        const auto &candidate = candidates[order[slot]];
        scratch.slots[order[slot]] = slot;
        out[slot].word.assign(candidate.first.source.slice(source));
        out[slot].weight = candidate.weight;
        out[slot].tag = candidate.tag;
        out[slot].positions.clear();
    }
    for (const auto &occurrence : table.occurrences) {
        if (const auto slot = scratch.slots[occurrence.candidate]; slot != unselected) {
            out[slot].positions.push_back(occurrence.position);
        }
    }
}

} // namespace neo_cppjieba::detail
