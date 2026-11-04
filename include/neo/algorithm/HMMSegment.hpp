#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/CharClass.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMMState.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/simd/Viterbi.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

// A reference HMM transition retains the selected predecessor along with its score.
struct HMMTransition {
    double weight;
    HMMState previous;
};

// Python and Rust consider only legal predecessors and select the higher state on a tie.
[[nodiscard]]
inline auto style_hmm_transition(const HMModel &model, const EmitProbabilities &weights, double emit,
                                 HMMState state) noexcept -> HMMTransition {
    assert(static_cast<size_t>(state) < kHMMStatesNum);
    constexpr auto predecessors =
        std::array{std::array{HMMState::E, HMMState::S}, std::array{HMMState::B, HMMState::M},
                   std::array{HMMState::B, HMMState::M}, std::array{HMMState::E, HMMState::S}};
    const auto states = predecessors[static_cast<size_t>(state)];
    const auto first = weights[static_cast<size_t>(states[0])] + model.get_trans_prob(states[0], state) + emit;
    const auto second = weights[static_cast<size_t>(states[1])] + model.get_trans_prob(states[1], state) + emit;
    return first > second ? HMMTransition{first, states[0]} : HMMTransition{second, states[1]};
}

/// Run the Viterbi algorithm on runes[begin..end) and emit each word with global offset pos.
template <WordEmitter Emit>
inline auto hmm_internal_cut(const HMModel &model, std::span<const Rune> runes, RuneIndex begin, RuneIndex end,
                             RuneIndex pos, const Emit &emit_word, SegmentScratch &scratch) -> void {
    assert_check([&] { return begin < end && end <= runes.size(); }, "HMMSegment: invalid Viterbi rune range");
    assert_check([&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
                 "HMMSegment: global word offsets overflow");
    constexpr auto Y = kHMMStatesNum; // 4 states: B, E, M, S
    const auto X = static_cast<size_t>(end - begin);
    // A valid Rune span already bounds the storage needed for each row of HMM states.
    static_assert(Y <= sizeof(Rune));
    assert_check([&] { return X <= std::numeric_limits<size_t>::max() / Y; },
                 "HMMSegment: input exceeds the Viterbi table size limit");

    // Scores need only the previous and current columns; paths keep one contiguous [position][state] row each.
    auto previous_weight = std::array<double, Y>{};
    // Every active row is overwritten before backtrace; retain the high-water size between HMM runs.
    if (scratch.hmm_path.size() < X) {
        scratch.hmm_path.resize(X);
    }
    // Keep the active row view stable after growth; Viterbi never resizes this storage.
    const auto path = std::span{scratch.hmm_path}.first(X);

    // ── Initialization (t = 0) ──────────────────────────────────────
    const auto first_emit = model.get_emit_probs(runes[to_index(begin)]);
    for (auto y = size_t{0}; y < Y; ++y) {
        previous_weight[y] = model.get_start_prob(static_cast<HMMState>(y)) + first_emit[y];
        assert_check([&] { return std::isfinite(previous_weight[y]); }, "HMMSegment: non-finite initial weight");
    }
    path[0].fill(HMMState::B);

    // ── Recursion (t = 1 .. X-1) ────────────────────────────────────
    if constexpr (compile_config::segmentation_style == SegmentationStyle::CPP) {
        // CppJieba floors scores at MIN_DOUBLE and defaults to the E predecessor. When every candidate
        // ties at the floor, as for runes absent from the emission table, this default decides the path.
        const auto emit_at = [&](size_t x) {
            return model.get_emit_probs(runes[to_index(begin + x)]);
        };
        previous_weight = simd::viterbi_forward(previous_weight, std::span{model.get_trans_probs()},
                                                simd::ViterbiFloor<HMMState>{.score = MIN_DOUBLE, .state = HMMState::E},
                                                emit_at, path);
    } else {
        auto current_weight = std::array<double, Y>{};
        for (auto x = size_t{1}; x < X; ++x) {
            const auto emit_probs = model.get_emit_probs(runes[to_index(begin + x)]);
            for (auto y = size_t{0}; y < Y; ++y) {
                const auto transition =
                    style_hmm_transition(model, previous_weight, emit_probs[y], static_cast<HMMState>(y));
                assert_check(
                    [&] { return static_cast<size_t>(transition.previous) < Y && std::isfinite(transition.weight); },
                    "HMMSegment: Viterbi must select a valid predecessor");
                current_weight[y] = transition.weight;
                path[x][y] = transition.previous;
            }
            previous_weight = current_weight;
        }
    }

    // ── Termination: pick the best final state (must be E or S) ─────
    auto last = X - 1;
    auto end_weight = previous_weight[static_cast<size_t>(HMMState::E)];
    auto end_state = HMMState::E;
    auto s_weight = previous_weight[static_cast<size_t>(HMMState::S)];
    if constexpr (compile_config::segmentation_style == SegmentationStyle::CPP) {
        if (s_weight > end_weight) {
            end_state = HMMState::S;
        }
    } else if (s_weight >= end_weight) {
        end_state = HMMState::S;
    }

    // ── Backtrace ───────────────────────────────────────────────────
    // Once a row's predecessor is read, that row is no longer needed for backtrace.
    // Reuse slot 0 for its decoded state so emitting words needs no separate status buffer.
    auto backtrace_state = end_state;
    for (auto x = last; x > 0; --x) {
        assert_check([&] { return static_cast<size_t>(backtrace_state) < Y; },
                     "HMMSegment: traceback state is out of range");
        const auto previous_state = path[x][static_cast<size_t>(backtrace_state)];
        path[x][0] = backtrace_state;
        backtrace_state = previous_state;
    }
    path[0][0] = backtrace_state;

    // Emit word ranges based on E/S boundaries.
    auto word_begin = begin;
    for (auto i = size_t{0}; i < X; ++i) {
        assert_check([&] { return static_cast<size_t>(path[i][0]) < Y; }, "HMMSegment: emitted state is out of range");
        const auto state = path[i][0];
        if constexpr (compile_config::segmentation_style != SegmentationStyle::CPP) {
            if (state == HMMState::B || state == HMMState::S) {
                word_begin = begin + static_cast<RuneIndex>(i);
            }
        }
        if (state == HMMState::E || state == HMMState::S) {
            emit_word(WordRange{static_cast<RuneIndex>(pos + word_begin), static_cast<RuneIndex>(pos + begin + i + 1)});
            word_begin = static_cast<RuneIndex>(begin + static_cast<RuneIndex>(i) + 1);
        }
    }
    assert_check([&] { return word_begin == end; }, "HMMSegment: words must cover the entire Viterbi range");
}

/// Find the end position of a consecutive ASCII letter sequence starting at `begin`.
/// Returns `begin` if runes[begin] is not a letter.
[[nodiscard]]
constexpr auto sequential_letter_end(std::span<const Rune> runes, RuneIndex begin, RuneIndex end) noexcept
    -> RuneIndex {
    assert_check([&] { return begin < end && end <= runes.size(); }, "HMMSegment: invalid ASCII letter range");
    auto r = runes[to_index(begin)];
    if (('a' > r || r > 'z') && ('A' > r || r > 'Z')) {
        return begin;
    }
    auto pos = begin + 1;
    while (pos < end) {
        r = runes[static_cast<size_t>(pos)];
        if (('a' <= r && r <= 'z') || ('A' <= r && r <= 'Z') || ('0' <= r && r <= '9')) {
            ++pos;
        } else {
            break;
        }
    }
    return static_cast<RuneIndex>(pos);
}

/// Find the end position of a consecutive number sequence starting at `begin`.
/// Returns `begin` if runes[begin] is not a digit.
[[nodiscard]]
constexpr auto number_end(std::span<const Rune> runes, RuneIndex begin, RuneIndex end) noexcept -> RuneIndex {
    assert_check([&] { return begin < end && end <= runes.size(); }, "HMMSegment: invalid ASCII number range");
    auto r = runes[to_index(begin)];
    if ('0' > r || r > '9') {
        return begin;
    }
    auto pos = begin + 1;
    while (pos < end) {
        r = runes[static_cast<size_t>(pos)];
        if (('0' <= r && r <= '9') || r == '.') {
            ++pos;
        } else {
            break;
        }
    }
    return static_cast<RuneIndex>(pos);
}

// Emit the reference HMM's alternating alphanumeric matches and non-Chinese gaps.
template <WordEmitter Emit>
inline auto style_hmm_emit_non_han(std::span<const Rune> runes, RuneIndex pos, const Emit &emit_word) -> void {
    auto begin = RuneIndex{0};
    while (begin < runes.size()) {
        auto end = begin;
        if (is_ascii_alphanumeric(runes[to_index(begin)])) {
            end = style_hmm_ascii_end(runes, begin);
        } else {
            do {
                ++end;
            } while (end < runes.size() && !is_ascii_alphanumeric(runes[to_index(end)]));
        }
        emit_word(WordRange{static_cast<RuneIndex>(pos + begin), static_cast<RuneIndex>(pos + end)});
        begin = end;
    }
}

// Isolate the reference HMM's Han blocks before running Viterbi, preserving source order.
template <WordEmitter Emit>
inline auto style_hmm_emit(const HMModel &model, std::span<const Rune> runes, RuneIndex pos, const Emit &emit_word,
                           SegmentScratch &scratch) -> void {
    auto begin = RuneIndex{0};
    while (begin < runes.size()) {
        const auto han = is_hmm_han(runes[to_index(begin)]);
        auto end = static_cast<RuneIndex>(begin + 1);
        while (end < runes.size() && is_hmm_han(runes[to_index(end)]) == han) {
            ++end;
        }
        if (!han) {
            style_hmm_emit_non_han(runes.subspan(to_index(begin), to_index(end - begin)), pos + begin, emit_word);
        } else if (end - begin == 1) {
            emit_word(WordRange{static_cast<RuneIndex>(pos + begin), static_cast<RuneIndex>(pos + end)});
        } else {
            hmm_internal_cut(model, runes, begin, end, pos, emit_word, scratch);
        }
        begin = end;
    }
}

/// Append HMM segmentation results for a separator-free segment, preserving ASCII runs as whole tokens.
template <WordEmitter Emit>
inline auto hmm_emit_one_segment(const HMModel &model, std::span<const Rune> runes, RuneIndex pos,
                                 const Emit &emit_word, SegmentScratch &scratch) -> void {
    assert_check([&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
                 "HMMSegment: global word offsets overflow");
    if (runes.empty()) {
        return;
    }

    if constexpr (compile_config::segmentation_style != SegmentationStyle::CPP) {
        style_hmm_emit(model, runes, pos, emit_word, scratch);
    } else {
        auto n = static_cast<RuneIndex>(runes.size());
        // One rune is always one word: ASCII grouping and a one-column Viterbi (final state E or S) agree.
        // MIX and SEARCH pass every isolated single-rune route word here, so skip both scans.
        if (n == 1) {
            emit_word(WordRange{pos, static_cast<RuneIndex>(pos + 1)});
            return;
        }

        auto left = RuneIndex{0};
        auto right = RuneIndex{0};

        while (right < n) {
            assert_check([&] { return left <= right; }, "HMMSegment: pending rune range is reversed");
            if (runes[to_index(right)] < 0x80) {
                // Flush pending Chinese characters to HMM before handling ASCII.
                if (left < right) {
                    hmm_internal_cut(model, runes, left, right, pos, emit_word, scratch);
                }
                left = right;

                // Group ASCII words and numeric runs so they are not split by the HMM state machine.
                auto end = sequential_letter_end(runes, left, n);
                if (end == left) {
                    end = number_end(runes, left, n);
                }
                if (end == left) {
                    end = left + 1;
                }
                assert_check([&] { return left < end && end <= n; }, "HMMSegment: ASCII scan must advance in bounds");
                emit_word(WordRange{static_cast<RuneIndex>(pos + left), static_cast<RuneIndex>(pos + end)});
                right = end;
                left = right;
            } else {
                // Runes absent from the model stay inside the current Viterbi span, as in CppJieba.
                ++right;
            }
        }

        // Flush the trailing Chinese run after the last ASCII span, if any.
        if (left < right) {
            hmm_internal_cut(model, runes, left, right, pos, emit_word, scratch);
        }
    }
}

/// Append emitted HMM words to a caller-owned vector without changing its existing prefix.
inline auto hmm_cut_one_segment(const HMModel &model, std::span<const Rune> runes, RuneIndex pos,
                                std::vector<WordRange> &range, SegmentScratch &scratch) -> void {
    const auto emit_word = [&](WordRange word) {
        range.push_back(word);
    };
    hmm_emit_one_segment(model, runes, pos, emit_word, scratch);
}

/// Append HMM segmentation results; the CPP style also keeps separator runes as standalone tokens.
inline auto hmm_cut_append(const HMModel &model, std::span<const Rune> runes, RuneIndex pos,
                           std::vector<WordRange> &range, SegmentScratch &scratch) -> void {
    assert_check([&] { return runes.size() <= static_cast<RuneIndex>(std::numeric_limits<RuneIndex>::max() - pos); },
                 "HMMSegment: global word offsets overflow");
    if constexpr (compile_config::segmentation_style != SegmentationStyle::CPP) {
        hmm_cut_one_segment(model, runes, pos, range, scratch);
    } else {
        for_each_separated(
            runes,
            [&](std::span<const Rune> segment, RuneIndex offset) {
                hmm_cut_one_segment(model, segment, static_cast<RuneIndex>(pos + offset), range, scratch);
            },
            [&](RuneIndex separator) {
                range.push_back({static_cast<RuneIndex>(pos + separator), static_cast<RuneIndex>(pos + separator + 1)});
            },
            scratch.separators);
    }
}

/// Stateless HMM-based segmentation operator.
///
/// HMM mode uses a Hidden Markov Model with the Viterbi algorithm to segment
/// Chinese text that is not covered by the dictionary. It assigns each character
/// one of four hidden states — B (Begin), E (End), M (Middle), S (Single) —
/// and uses word boundaries indicated by E/S states to split the input.
///
/// ASCII sequences (letters and numbers) are handled by dedicated rules that
/// group them into single tokens before HMM processing.
///
/// This is a purely static utility — no instance state, no ownership of the
/// model. The HMModel is taken as a const reference parameter; the dictionary is unused.
struct HMMSegment {
    // Replace output and reuse predecessor storage across HMM runs and calls.
    static auto cut_into(const DictTrie &, const HMModel &model, std::span<const Rune> runes,
                         std::vector<WordRange> &range, detail::SegmentScratch &scratch) -> void {
        assert_check([&] { return detail::size_fits_capacity<RuneIndex>(runes.size()); },
                     "HMMSegment: input exceeds the word-range limit");
        range.clear();
        // A partition has at most one word per rune, so appending never reallocates.
        range.reserve(runes.size());
        detail::hmm_cut_append(model, runes, 0, range, scratch);
    }
};

static_assert(SegmentPolicy<HMMSegment>);

} // namespace neo_cppjieba::detail
