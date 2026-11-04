#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/Separator.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

/// Check a word partition whose rune span starts at a caller-provided global offset.
[[nodiscard]]
inline auto valid_segment_partition_at(std::span<const WordRange> words, size_t rune_count, RuneIndex pos) noexcept
    -> bool {
    auto next = pos;
    for (const auto &word : words) {
        if (word.begin != next || word.begin >= word.end || static_cast<RuneIndex>(word.end - pos) > rune_count) {
            return false;
        }
        next = word.end;
    }
    return static_cast<RuneIndex>(next - pos) == rune_count;
}

/// Check that local word ranges form a nonempty-token partition of the rune span.
[[nodiscard]]
inline auto valid_segment_partition(std::span<const WordRange> words, size_t rune_count) noexcept -> bool {
    return valid_segment_partition_at(words, rune_count, 0);
}

/// Split runes at the default separators, as the CPP style does before every dictionary or HMM pass.
/// on_text(segment, offset) receives each separator-free span, possibly empty, with its offset in runes;
/// on_separator(offset) receives each separator rune between them, in input order.
/// The separator list is caller-owned scratch; the callbacks must not modify it.
template <SegmentVisitor OnText, SeparatorVisitor OnSeparator>
inline auto for_each_separated(std::span<const Rune> runes, OnText &&on_text, OnSeparator &&on_separator,
                               std::vector<RuneIndex> &separators) -> void {
    get_pre_filter_separators(runes, separators);
    // The final entry is the runes.size() sentinel, so every separator is followed by one more text span.
    auto begin = RuneIndex{0};
    for (auto i = size_t{0};; ++i) {
        const auto end = separators[i];
        on_text(runes.subspan(to_index(begin), to_index(end - begin)), begin);
        if (i + 1 == separators.size()) {
            return;
        }
        on_separator(end);
        begin = end + 1;
    }
}

// Convenience entry that allocates a fresh result and scratch, for tests and diagnostics.
template <SegmentPolicy Policy>
[[nodiscard]]
auto cut(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes) -> std::vector<WordRange> {
    auto out = std::vector<WordRange>{};
    auto scratch = SegmentScratch{};
    Policy::cut_into(dict, model, runes, out, scratch);
    return out;
}

} // namespace neo_cppjieba::detail
