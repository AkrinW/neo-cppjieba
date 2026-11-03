#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/CharClass.hpp"
#include "neo/algorithm/HMMSegment.hpp"
#include "neo/concept/Segment.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

// Search retains short-word membership during the existing dictionary traversal.
enum class StyleMatchMode { PathOnly, Search };

// Reference MP scoring uses a frequency of one only when no dictionary edge exists.
template <StyleMatchMode mode>
inline auto style_build_route(const DictTrie &dict, std::span<const Rune> runes, WordWeight missing_weight,
                              SegmentScratch &scratch) -> void {
    assert(dict.freq_sum() > 0);
    auto &route = scratch.route;
    if (route.size() < runes.size()) {
        route.resize(runes.size());
    }
    if constexpr (mode == StyleMatchMode::Search) {
        if (scratch.search_subwords.size() < runes.size()) {
            scratch.search_subwords.resize(runes.size());
        }
    }
    for (auto i = runes.size(); i > 0;) {
        --i;
        if constexpr (mode == StyleMatchMode::Search) {
            scratch.search_subwords[i] = {};
        }
        auto best = MPNode{-std::numeric_limits<WordWeight>::infinity(), 0};
        dict.for_each_match_from(runes, static_cast<RuneIndex>(i), [&](RuneIndex end, const DictUnit &word) {
            if constexpr (mode == StyleMatchMode::Search) {
                auto &subwords = scratch.search_subwords[i];
                subwords.bigram |= end - i == 2;
                subwords.trigram |= end - i == 3;
            }
            const auto weight = word.weight + (end < runes.size() ? route[to_index(end)].weight : WordWeight{0});
            if (weight > best.weight || (weight == best.weight && end > best.next_pos)) {
                best = {weight, end};
            }
        });
        if (best.next_pos == 0) {
            const auto end = static_cast<RuneIndex>(i + 1);
            best = {missing_weight + (end < runes.size() ? route[to_index(end)].weight : WordWeight{0}), end};
        }
        assert(std::isfinite(best.weight) && best.next_pos > i && best.next_pos <= runes.size());
        route[i] = best;
    }
}

// HMM must not undo MP's single-character decomposition of a known dictionary word.
template <bool hmm, WordEmitter Emit>
inline auto style_emit_singletons(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes,
                                  WordRange run, const Emit &emit, SegmentScratch &scratch) -> void {
    assert(run.begin < run.end && run.end <= runes.size());
    if constexpr (!hmm) {
        emit(run);
        return;
    }
    if (run.size() == 1) {
        emit(run);
        return;
    }
    const auto text = run.slice(runes);
    if (!dict.find(text).has_value()) {
        hmm_emit_one_segment(model, text, run.begin, emit, scratch);
        return;
    }
    for (auto i = run.begin; i < run.end; ++i) {
        emit(WordRange{i, static_cast<RuneIndex>(i + 1)});
    }
}

// Consume the reusable MP route, joining ASCII without HMM and recognizing unknown runs with HMM.
template <bool hmm, StyleMatchMode mode, WordEmitter Emit>
inline auto style_emit_route(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes,
                             WordWeight missing_weight, const Emit &emit, SegmentScratch &scratch) -> void {
    style_build_route<mode>(dict, runes, missing_weight, scratch);
    const auto &route = scratch.route;
    auto begin = RuneIndex{0};
    while (begin < runes.size()) {
        auto end = route[to_index(begin)].next_pos;
        if (end - begin != 1 || (!hmm && !is_ascii_alphanumeric(runes[to_index(begin)]))) {
            emit(WordRange{begin, end});
            begin = end;
            continue;
        }
        while (end < runes.size() && route[to_index(end)].next_pos == end + 1
               && (hmm || is_ascii_alphanumeric(runes[to_index(end)]))) {
            ++end;
        }
        style_emit_singletons<hmm>(dict, model, runes, {begin, end}, emit, scratch);
        begin = end;
    }
}

// Dictionary blocks use the selected character class; unmatched text preserves CRLF as one token.
template <bool hmm, StyleMatchMode mode, typename Emit>
    requires((mode == StyleMatchMode::PathOnly && WordEmitter<Emit>)
             || (mode == StyleMatchMode::Search && BlockWordEmitter<Emit>))
inline auto style_emit_blocks(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes, const Emit &emit,
                              SegmentScratch &scratch) -> void {
    // All dictionary blocks in this call share the same fallback weight.
    const auto missing_weight = static_cast<WordWeight>(-std::log(static_cast<double>(dict.freq_sum())));
    auto begin = RuneIndex{0};
    while (begin < runes.size()) {
        const auto matched = is_style_dictionary_rune(runes[to_index(begin)]);
        auto end = static_cast<RuneIndex>(begin + 1);
        while (end < runes.size() && is_style_dictionary_rune(runes[to_index(end)]) == matched) {
            ++end;
        }
        if (matched) {
            const auto emit_local = [&](WordRange word) {
                const auto global =
                    WordRange{static_cast<RuneIndex>(begin + word.begin), static_cast<RuneIndex>(begin + word.end)};
                if constexpr (mode == StyleMatchMode::Search) {
                    emit(global, begin);
                } else {
                    emit(global);
                }
            };
            style_emit_route<hmm, mode>(dict, model, runes.subspan(to_index(begin), to_index(end - begin)),
                                        missing_weight, emit_local, scratch);
        } else {
            while (begin < end) {
                auto next = static_cast<RuneIndex>(begin + 1);
                if (runes[to_index(begin)] == U'\r' && next < end && runes[to_index(next)] == U'\n') {
                    ++next;
                }
                if constexpr (mode == StyleMatchMode::Search) {
                    emit(WordRange{begin, next}, begin);
                } else {
                    emit(WordRange{begin, next});
                }
                begin = next;
            }
        }
        begin = end;
    }
}

// Reference precise mode for the RUST and PYTHON styles; retains the caller's storage across calls.
template <bool hmm>
struct StyledMixSegment {
    static auto cut_into(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes,
                         std::vector<WordRange> &out, SegmentScratch &scratch) -> void {
        out.clear();
        out.reserve(runes.size());
        const auto emit = [&](WordRange word) {
            out.push_back(word);
        };
        style_emit_blocks<hmm, StyleMatchMode::PathOnly>(dict, model, runes, emit, scratch);
    }
};

static_assert(SegmentPolicy<StyledMixSegment<true>> && SegmentPolicy<StyledMixSegment<false>>);

// Rust search adds alphabetic parts of connector-separated compounds before dictionary grams.
inline auto style_emit_compound_parts(std::span<const Rune> runes, WordRange word, std::vector<WordRange> &out)
    -> void {
    const auto text = word.slice(runes);
    if (!std::ranges::any_of(text, is_rust_connector)) {
        return;
    }
    auto begin = word.begin;
    while (begin < word.end) {
        auto end = begin;
        while (end < word.end && !is_rust_connector(runes[to_index(end)])) {
            ++end;
        }
        if (end - begin >= 2
            && std::ranges::any_of(runes.subspan(to_index(begin), to_index(end - begin)), is_ascii_letter)) {
            out.push_back({begin, end});
        }
        begin = end < word.end ? static_cast<RuneIndex>(end + 1) : end;
    }
}

// Reference search emits two-rune matches, then three-rune matches, then the precise-mode word.
inline auto style_emit_search_word(std::span<const Rune> runes, WordRange word, RuneIndex block_begin,
                                   std::span<const SearchSubwords> subwords, std::vector<WordRange> &out) -> void {
    assert(word.begin < word.end && word.end <= runes.size());
    // Emitted ranges are global; membership rows belong to the current dictionary block.
    assert(word.size() <= 2
           || (word.begin >= block_begin && static_cast<RuneIndex>(word.end - block_begin) <= subwords.size()));
    if constexpr (compile_config::segmentation_style == SegmentationStyle::RUST) {
        style_emit_compound_parts(runes, word, out);
    }
    for (const auto width : {RuneIndex{2}, RuneIndex{3}}) {
        if (word.size() <= width) {
            continue;
        }
        for (auto i = word.begin; i <= word.end - width; ++i) {
            const auto matches = subwords[to_index(i - block_begin)];
            if (width == 2 ? matches.bigram : matches.trigram) {
                out.push_back({i, static_cast<RuneIndex>(i + width)});
            }
        }
    }
    out.push_back(word);
}

// Search shares precise-mode boundaries and emits directly into the final range array.
template <bool hmm>
struct StyledQuerySegment {
    static auto cut_into(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes,
                         std::vector<WordRange> &out, SegmentScratch &scratch) -> void {
        out.clear();
        out.reserve(runes.size());
        const auto emit = [&](WordRange word, RuneIndex block_begin) {
            style_emit_search_word(runes, word, block_begin, scratch.search_subwords, out);
        };
        style_emit_blocks<hmm, StyleMatchMode::Search>(dict, model, runes, emit, scratch);
    }
};

static_assert(SegmentPolicy<StyledQuerySegment<true>> && SegmentPolicy<StyledQuerySegment<false>>);

// Python FULL keeps whitespace matches and the intervening gaps, including empty gaps.
inline auto style_python_full_unmatched(std::span<const Rune> runes, RuneIndex pos, std::vector<WordRange> &out)
    -> void {
    auto gap = RuneIndex{0};
    auto i = RuneIndex{0};
    while (i < runes.size()) {
        if (!is_python_space(runes[to_index(i)])) {
            ++i;
            continue;
        }
        out.push_back({static_cast<RuneIndex>(pos + gap), static_cast<RuneIndex>(pos + i)});
        auto end = static_cast<RuneIndex>(i + 1);
        if (runes[to_index(i)] == U'\r' && end < runes.size() && runes[to_index(end)] == U'\n') {
            ++end;
        }
        out.push_back({static_cast<RuneIndex>(pos + i), static_cast<RuneIndex>(pos + end)});
        gap = end;
        i = end;
    }
    out.push_back({static_cast<RuneIndex>(pos + gap), static_cast<RuneIndex>(pos + runes.size())});
}

// Python FULL buffers English words; overlapping matches extend ranges instead of duplicating source text.
inline auto style_python_full_block(const DictTrie &dict, std::span<const Rune> runes, RuneIndex pos,
                                    std::vector<WordRange> &out, SegmentScratch &scratch) -> void {
    auto previous_end = RuneIndex{0};
    auto english = WordRange{0, 0};
    const auto emit = [&](WordRange word) {
        out.push_back({static_cast<RuneIndex>(pos + word.begin), static_cast<RuneIndex>(pos + word.end)});
    };
    const auto flush_english = [&] {
        if (english.begin < english.end) {
            emit(english);
            english = {0, 0};
        }
    };
    auto &matches = scratch.full_matches;
    // NOLINTNEXTLINE(bugprone-too-small-loop-variable): every entry point asserts the rune count fits RuneIndex.
    for (auto i = RuneIndex{0}; i < runes.size(); ++i) {
        if (!is_ascii_alphanumeric(runes[to_index(i)])) {
            flush_english();
        }
        matches.clear();
        dict.for_each_match_from(runes, i, [&](RuneIndex end, const DictUnit &) { matches.push_back({i, end}); });
        if (matches.empty()) {
            matches.push_back({i, static_cast<RuneIndex>(i + 1)});
        }
        if (matches.size() != 1 || i < previous_end) {
            for (const auto word : matches) {
                if (word.size() > 1) {
                    emit(word);
                    previous_end = word.end;
                }
            }
            continue;
        }
        const auto word = matches.front();
        if (is_ascii_alphanumeric(runes[to_index(i)])) {
            if (i > english.end) {
                flush_english();
            }
            english = english.begin == english.end ? word : WordRange{english.begin, std::max(english.end, word.end)};
        } else {
            emit(word);
        }
        previous_end = word.end;
    }
    flush_english();
}

// Outside Rust FULL's CJK blocks, ASCII alphanumerics, plus, hash and LF form runs.
[[nodiscard]]
constexpr auto is_rust_full_run(Rune rune) noexcept -> bool {
    return is_ascii_alphanumeric(rune) || rune == U'+' || rune == U'#' || rune == U'\n';
}

// Rust FULL emits other unmatched characters individually, without Python's empty tokens.
inline auto style_rust_full_unmatched(std::span<const Rune> runes, RuneIndex pos, std::vector<WordRange> &out) -> void {
    auto begin = RuneIndex{0};
    while (begin < runes.size()) {
        auto end = static_cast<RuneIndex>(begin + 1);
        if (is_rust_full_run(runes[to_index(begin)])) {
            while (end < runes.size() && is_rust_full_run(runes[to_index(end)])) {
                ++end;
            }
        }
        out.push_back({static_cast<RuneIndex>(pos + begin), static_cast<RuneIndex>(pos + end)});
        begin = end;
    }
}

// FULL uses separate reference contracts: Rust emits every CJK match; Python suppresses covered singletons.
struct StyledFullSegment {
    static auto cut_into(const DictTrie &dict, const HMModel &, std::span<const Rune> runes,
                         std::vector<WordRange> &out, SegmentScratch &scratch) -> void {
        out.clear();
        out.reserve(runes.size());
        const auto classify = [](Rune rune) {
            if constexpr (compile_config::segmentation_style == SegmentationStyle::RUST) {
                return is_rust_cjk(rune);
            } else {
                return is_style_dictionary_rune(rune);
            }
        };
        auto begin = RuneIndex{0};
        while (begin < runes.size()) {
            const auto matched = classify(runes[to_index(begin)]);
            auto end = static_cast<RuneIndex>(begin + 1);
            while (end < runes.size() && classify(runes[to_index(end)]) == matched) {
                ++end;
            }
            const auto block = runes.subspan(to_index(begin), to_index(end - begin));
            if constexpr (compile_config::segmentation_style == SegmentationStyle::RUST) {
                if (matched) {
                    for (auto i = begin; i < end; ++i) {
                        dict.for_each_match_from(
                            runes.first(to_index(end)), i,
                            [&](RuneIndex word_end, const DictUnit &) { out.push_back({i, word_end}); });
                    }
                } else {
                    style_rust_full_unmatched(block, begin, out);
                }
            } else {
                if (matched) {
                    style_python_full_block(dict, block, begin, out, scratch);
                } else {
                    style_python_full_unmatched(block, begin, out);
                }
            }
            begin = end;
        }
    }
};

static_assert(SegmentPolicy<StyledFullSegment>);

} // namespace neo_cppjieba::detail
