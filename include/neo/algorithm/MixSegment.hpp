#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/HMMSegment.hpp"
#include "neo/algorithm/MPSegment.hpp"
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

/// Visit the MP route built for a separator-free segment as local word ranges, in input order.
/// With HMM, each run of single-rune route words that are not user-dictionary singletons is
/// re-segmented by HMM so out-of-vocabulary words can be recovered. MIX and SEARCH share this walk.
template <bool hmm, WordEmitter Emit>
inline auto mix_emit_route(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes, const Emit &emit,
                           SegmentScratch &scratch) -> void {
    if constexpr (!hmm) {
        mp_emit_route(runes.size(), emit, scratch);
    } else {
        assert(scratch.route.size() >= runes.size());
        // HMM uses its own path buffer, so the MP route remains valid throughout this traversal.
        const auto &route = scratch.route;
        auto begin = RuneIndex{0};
        while (begin < runes.size()) {
            const auto end = route[to_index(begin)].next_pos;
            assert(begin < end && end <= runes.size());

            // Multi-character word or user-dict single character → emit directly.
            if (end - begin > 1 || dict.is_user_dict_single_chinese_word(runes[to_index(begin)])) {
                emit(WordRange{begin, end});
                begin = end;
                continue;
            }

            // Collect consecutive single-character words that are not user-dict words.
            auto run_end = end;
            while (run_end < runes.size() && route[to_index(run_end)].next_pos == run_end + 1
                   && !dict.is_user_dict_single_chinese_word(runes[to_index(run_end)])) {
                ++run_end;
            }
            // The outer cut already removed separators; HMM keeps its ASCII handling.
            hmm_emit_one_segment(model, runes.subspan(to_index(begin), to_index(run_end - begin)), begin, emit,
                                 scratch);
            begin = run_end;
        }
    }
}

/// Append MIX results at global offset pos, keeping separator runes as standalone tokens.
template <bool hmm>
inline auto mix_cut_append(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes, RuneIndex pos,
                           std::vector<WordRange> &out, SegmentScratch &scratch) -> void {
    if constexpr (!hmm) {
        mp_cut_append(dict, runes, pos, out, scratch);
    } else {
        assert_check(
            [&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
            "MixSegment: global word offsets overflow");
        for_each_separated(
            runes,
            [&](std::span<const Rune> segment, RuneIndex offset) {
                if (segment.empty()) {
                    return;
                }
                const auto base = static_cast<RuneIndex>(pos + offset);
                mp_build_route_from_matches<MPMatchMode::PathOnly>(dict, segment, scratch);
                mix_emit_route<true>(
                    dict, model, segment,
                    [&](WordRange word) {
                        out.push_back(
                            {static_cast<RuneIndex>(base + word.begin), static_cast<RuneIndex>(base + word.end)});
                    },
                    scratch);
            },
            [&](RuneIndex separator) {
                out.push_back({static_cast<RuneIndex>(pos + separator), static_cast<RuneIndex>(pos + separator + 1)});
            },
            scratch.separators);
    }
}

/// Stateless mixed-mode segmentation operator.
///
/// Mix mode ("混合模式") combines maximum-probability (MP) segmentation with
/// HMM-based segmentation: runs of consecutive single characters on the MP route
/// that are NOT user-dictionary single-char words are re-segmented by HMM
/// to discover OOV (out-of-vocabulary) words. Without HMM it equals MP.
///
/// This is the default and recommended segmentation mode in jieba — it offers
/// the best balance between dictionary-based precision and OOV recall.
template <bool hmm = true>
struct MixSegment {
    // Replace output while keeping MP and HMM storage alive for later segments and calls.
    static auto cut_into(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes,
                         std::vector<WordRange> &result, detail::SegmentScratch &scratch) -> void {
        assert_check([&] { return runes.size() <= std::numeric_limits<RuneIndex>::max(); },
                     "MixSegment: input exceeds the word-range limit");
        result.clear();
        result.reserve(runes.size() / 2);
        detail::mix_cut_append<hmm>(dict, model, runes, 0, result, scratch);
    }
};

static_assert(SegmentPolicy<MixSegment<true>> && SegmentPolicy<MixSegment<false>>);

} // namespace neo_cppjieba::detail
