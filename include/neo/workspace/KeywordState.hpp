#pragma once

#include "neo/third_party/gtl.hpp"
#include "neo/util/RuneString.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/TokenView.hpp"
#include "neo/workspace/Workspace.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace neo_cppjieba::detail {

// A distinct candidate word; ids follow first occurrence, which also breaks score ties.
struct KeywordCandidate {
    TokenPosition first{};
    double weight{0.0};
    PosTag tag;

    // The word as code points of the decoded input it was collected from.
    [[nodiscard]]
    auto word(std::span<const Rune> runes) const noexcept -> std::u32string_view {
        return rune_view(first.runes.slice(runes));
    }
};

struct KeywordOccurrence {
    size_t candidate;
    TokenPosition position;
};

// Distinct candidate words and every occurrence in input order, filled once and read by each later step.
// Candidate keys in index borrow the decoded runes, so the index is cleared before those runes are reused.
struct CandidateTable {
    gtl::flat_hash_map<std::u32string_view, size_t, RuneStringHash, std::equal_to<>> index;
    std::vector<KeywordCandidate> candidates;
    std::vector<KeywordOccurrence> occurrences;
};

// A directed TextRank edge between node ranks, which order words as CppJieba's std::map<std::string> does.
struct KeywordEdge {
    size_t from;
    size_t to;
    double weight;
};

// TextRank's graph and score buffers. by_word lists candidate ids in rank order; ranks is its inverse.
struct TextRankScratch {
    std::vector<size_t> by_word;
    std::vector<size_t> ranks;
    std::vector<KeywordEdge> edges;
    std::vector<size_t> edge_begin;
    std::vector<double> out_sums;
    std::vector<double> scores;
};

// Top-N selection buffers: candidate ids ordered by weight, and each candidate's output slot.
struct SelectionScratch {
    std::vector<size_t> order;
    std::vector<size_t> slots;
};

// Per-call keyword extraction buffers behind the opaque KeywordWorkspace; capacity is retained across calls.
// Each algorithm receives only the group it reads or writes.
struct KeywordState {
    Workspace segment;
    UnicodeWithOffset decoded;
    std::vector<WordRange> ranges;
    CandidateTable table;
    TextRankScratch text_rank;
    SelectionScratch selection;
};

} // namespace neo_cppjieba::detail
