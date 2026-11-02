#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

/// Stateless full-mode segmentation operator.
///
/// Full mode ("全模式") outputs ALL possible dictionary words present in the
/// input sentence. Single characters that are not covered by any dictionary
/// word are also emitted as fallback tokens.
struct FullSegment {
    // Replace output; only separator storage is reused between calls.
    static auto cut_into(const DictTrie &dict, const HMModel &, std::span<const Rune> runes,
                         std::vector<WordRange> &range, detail::SegmentScratch &scratch) -> void {
        assert(detail::size_fits_capacity<RuneIndex>(runes.size()));
        range.clear();
        // Overlapping matches can exceed one word per rune; this covers typical input without regrowth.
        range.reserve(runes.size());
        detail::for_each_separated(
            runes,
            [&](std::span<const Rune> segment, RuneIndex offset) { cut_one_segment(dict, segment, offset, range); },
            [&](RuneIndex separator) { range.push_back({separator, static_cast<RuneIndex>(separator + 1)}); },
            scratch.separators);
    }

private:
    /// Emit all dictionary matches inside one separator-free segment, plus uncovered single-rune fallbacks.
    static auto cut_one_segment(const DictTrie &dict, std::span<const Rune> runes, RuneIndex pos,
                                std::vector<WordRange> &result) -> void {
        // max_covered tracks the furthest rune position covered by previously emitted dictionary words; used to decide
        // whether to emit single-char fallback tokens.
        auto max_covered = size_t{0};

        for (auto i = size_t{0}; i < runes.size(); ++i) {
            const auto single_end = static_cast<RuneIndex>(i + 1);
            auto longest_end = single_end;

            // Keep every multi-rune match; overlapping words are the point of full mode.
            dict.for_each_match_from(runes, static_cast<RuneIndex>(i), [&](RuneIndex end, const DictUnit &) {
                if (end == single_end) {
                    return;
                }
                result.push_back(WordRange{static_cast<RuneIndex>(pos + i), static_cast<RuneIndex>(pos + end)});
                longest_end = end;
            });
            // Without a longer match, emit the single rune unless an earlier word already covers it.
            if (longest_end == single_end && max_covered <= i) {
                result.push_back(WordRange{static_cast<RuneIndex>(pos + i), static_cast<RuneIndex>(pos + single_end)});
            }

            // Matches arrive from shortest to longest, so the last one extends farthest.
            max_covered = std::max(max_covered, static_cast<size_t>(longest_end));
        }
    }
};

static_assert(SegmentPolicy<FullSegment>);

} // namespace neo_cppjieba::detail
