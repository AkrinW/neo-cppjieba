#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/FullSegment.hpp"
#include "neo/algorithm/HMMSegment.hpp"
#include "neo/algorithm/MixSegment.hpp"
#include "neo/algorithm/QuerySegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/algorithm/StyledSegment.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/options/CutMode.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

// The only place where a runtime CutMode meets the compile-time SegmentationStyle.
// Each mode resolves to one algorithm family here; facades never branch on the style.

namespace neo_cppjieba::detail {

// Rune indices must fit WordRange, with room for the trailing offset sentinel.
inline auto assert_rune_count(size_t rune_count) noexcept -> void {
    assert_check(
        [&] { return size_fits_capacity<RuneIndex>(rune_count) && rune_count < std::numeric_limits<size_t>::max(); },
        "Jieba: input has {} runes, exceeding the configured index range", rune_count);
}

// CppJieba's algorithms, built on the shared MP route and separator split.
struct CppSegmentFamily {
    // Precise mode: MIX with HMM, MIX_NO_HMM and MP without.
    template <bool hmm>
    using Precise = MixSegment<hmm>;
    template <bool hmm>
    using Search = QuerySegment<hmm>;
    using Full = FullSegment;
    using Hmm = HMMSegment;
};

// Reference rules of jieba-rs and Python jieba; HMMSegment selects its style-specific details internally.
struct StyledSegmentFamily {
    template <bool hmm>
    using Precise = StyledMixSegment<hmm>;
    template <bool hmm>
    using Search = StyledQuerySegment<hmm>;
    using Full = StyledFullSegment;
    using Hmm = HMMSegment;
};

static_assert(SegmentFamily<CppSegmentFamily> && SegmentFamily<StyledSegmentFamily>);

// The build-wide family; unselected families are never instantiated beyond the checks above.
using ActiveSegmentFamily = std::conditional_t<compile_config::segmentation_style == SegmentationStyle::CPP,
                                               CppSegmentFamily, StyledSegmentFamily>;

// Partition modes cover every rune once; full and search modes may emit overlapping words.
[[nodiscard]]
inline auto valid_segment_result(CutMode mode, std::span<const WordRange> words, size_t rune_count) noexcept -> bool {
    if (mode == CutMode::MIX || mode == CutMode::MIX_NO_HMM || mode == CutMode::HMM || mode == CutMode::MP) {
        return valid_segment_partition(words, rune_count);
    }
    if (rune_count == 0) {
        return words.empty();
    }
    if (words.empty()) {
        // Rust FULL emits nothing for input without dictionary-block or ASCII runs.
        return compile_config::segmentation_style == SegmentationStyle::RUST && mode == CutMode::FULL;
    }
    return std::ranges::all_of(words, [=](const WordRange &word) {
        // Python FULL keeps the empty gaps between whitespace matches.
        const auto allow_empty =
            compile_config::segmentation_style == SegmentationStyle::PYTHON && mode == CutMode::FULL;
        return (word.begin < word.end || (allow_empty && word.begin == word.end)) && word.end <= rune_count;
    });
}

// Replace out with the words of a validated mode; out and scratch keep their capacity.
inline auto segment_into(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes, CutMode mode,
                         std::vector<WordRange> &out, SegmentScratch &scratch) -> void {
    using Family = ActiveSegmentFamily;
    switch (mode) {
        case CutMode::MIX:
            Family::Precise<true>::cut_into(dict, model, runes, out, scratch);
            break;
        // MP and MIX_NO_HMM are the same route in every style.
        case CutMode::MIX_NO_HMM:
        case CutMode::MP:
            Family::Precise<false>::cut_into(dict, model, runes, out, scratch);
            break;
        case CutMode::FULL:
            Family::Full::cut_into(dict, model, runes, out, scratch);
            break;
        case CutMode::SEARCH:
            Family::Search<true>::cut_into(dict, model, runes, out, scratch);
            break;
        case CutMode::SEARCH_NO_HMM:
            Family::Search<false>::cut_into(dict, model, runes, out, scratch);
            break;
        case CutMode::HMM:
            Family::Hmm::cut_into(dict, model, runes, out, scratch);
            break;
        default:
            assert_check([] { return false; }, "Unchecked Jieba cut mode reached internal dispatch");
            std::unreachable();
    }
    assert_check([&] { return valid_segment_result(mode, out, runes.size()); },
                 "Jieba: invalid segmentation result for {} runes", runes.size());
}

} // namespace neo_cppjieba::detail
