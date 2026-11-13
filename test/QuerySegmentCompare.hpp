#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/HMMSegment.hpp"
#include "neo/algorithm/MPSegment.hpp"
#include "neo/algorithm/MixSegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/algorithm/Separator.hpp"
#include "neo/resource/Dag.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <span>
#include <vector>

namespace neo_cppjieba::test {

// The comparison helpers exercise internal segmenters directly.
using namespace neo_cppjieba::detail;

namespace detail {

template <bool hmm>
inline
auto append_sub_words_by_lookup(const DictTrie &dict, std::span<const Rune> runes, WordRange word,
                                std::vector<WordRange> &result) -> void {
    auto len = word.size();

    if (len > 2) {
        for (auto i = RuneIndex{0}; i + 2 <= len; ++i) {
            auto sub = runes.subspan(word.begin + i, 2);
            if (dict.find(sub).has_value()) {
                result.push_back(
                    WordRange{static_cast<RuneIndex>(word.begin + i), static_cast<RuneIndex>(word.begin + i + 2)});
            }
        }
    }

    if (len > 3) {
        for (auto i = RuneIndex{0}; i + 3 <= len; ++i) {
            auto sub = runes.subspan(word.begin + i, 3);
            if (dict.find(sub).has_value()) {
                result.push_back(
                    WordRange{static_cast<RuneIndex>(word.begin + i), static_cast<RuneIndex>(word.begin + i + 3)});
            }
        }
    }

    result.push_back(word);
}

inline
auto has_dag_edge(const Dag &dag, RuneIndex begin, RuneIndex end) -> bool {
    for (auto &&edge : dag.get_edges(begin)) {
        if (edge.next_pos == end) {
            return true;
        }
    }
    return false;
}

inline
auto append_sub_words_from_dag(const Dag &dag, RuneIndex segment_offset, WordRange word, std::vector<WordRange> &result)
    -> void {
    auto len = word.size();
    auto local_begin = word.begin - segment_offset;

    if (len > 2) {
        for (auto i = RuneIndex{0}; i + 2 <= len; ++i) {
            auto begin = local_begin + i;
            auto end = begin + 2;
            if (has_dag_edge(dag, begin, end)) {
                result.push_back(
                    WordRange{static_cast<RuneIndex>(word.begin + i), static_cast<RuneIndex>(word.begin + i + 2)});
            }
        }
    }

    if (len > 3) {
        for (auto i = RuneIndex{0}; i + 3 <= len; ++i) {
            auto begin = local_begin + i;
            auto end = begin + 3;
            if (has_dag_edge(dag, begin, end)) {
                result.push_back(
                    WordRange{static_cast<RuneIndex>(word.begin + i), static_cast<RuneIndex>(word.begin + i + 3)});
            }
        }
    }

    result.push_back(word);
}

// Reference MP over a materialized DAG: the library builds the same route directly from trie matches.
inline
auto mp_words_from_dag(const DictTrie &dict, const Dag &dag) -> std::vector<WordRange> {
    auto scratch = neo_cppjieba::detail::SegmentScratch{};
    const auto for_each_edge = [&](RuneIndex begin, const auto &emit) {
        for (const auto &edge : dag.get_edges(begin)) {
            emit(edge.next_pos, edge.weight);
        }
    };
    neo_cppjieba::detail::mp_build_route(dict, dag.size(), for_each_edge, scratch);
    auto words = std::vector<WordRange>{};
    neo_cppjieba::detail::mp_emit_route(dag.size(), [&](WordRange word) { words.push_back(word); }, scratch);
    return words;
}

template <bool hmm>
inline
auto append_mix_words(const DictTrie &dict, const HMModel &model, std::vector<WordRange> &result,
                      const std::vector<WordRange> &mp_words, std::span<const Rune> runes, RuneIndex pos) -> void {
    if constexpr (!hmm) {
        for (const auto &word : mp_words) {
            result.push_back(
                WordRange{static_cast<RuneIndex>(pos + word.begin), static_cast<RuneIndex>(pos + word.end)});
        }
        return;
    }

    auto i = size_t{0};
    while (i < mp_words.size()) {
        const auto &word = mp_words[i];
        if (word.size() > 1 || (word.size() == 1 && dict.is_user_dict_single_chinese_word(runes[word.begin]))) {
            result.push_back(
                WordRange{static_cast<RuneIndex>(pos + word.begin), static_cast<RuneIndex>(pos + word.end)});
            ++i;
            continue;
        }

        auto j = i;
        while (j < mp_words.size() && mp_words[j].size() == 1
               && !dict.is_user_dict_single_chinese_word(runes[mp_words[j].begin])) {
            ++j;
        }

        auto run_begin = mp_words[i].begin;
        auto run_end = mp_words[j - 1].end;
        auto scratch = neo_cppjieba::detail::SegmentScratch{};
        neo_cppjieba::detail::hmm_cut_append(model, runes.subspan(run_begin, run_end - run_begin), pos + run_begin,
                                             result, scratch);

        i = j;
    }
}

template <bool hmm>
struct BufferedMixResult {
    struct SegmentDag {
        RuneIndex offset{0};
        Dag dag;

        [[nodiscard]]
        auto end() const noexcept -> RuneIndex {
            return offset + static_cast<RuneIndex>(dag.size());
        }
    };

    std::vector<WordRange> words;
    std::vector<SegmentDag> dags;
};

template <bool hmm>
inline
auto mix_cut_with_dag_buffered(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes)
    -> BufferedMixResult<hmm> {
    auto result = BufferedMixResult<hmm>{};
    result.words.reserve(runes.size() / 2);

    auto segments = get_pre_filter_separators(runes);
    auto pos = RuneIndex{0};

    auto cut_one = [&](std::span<const Rune> segment_runes, RuneIndex segment_pos) {
        if (segment_runes.empty()) {
            return;
        }
        auto dag = dict.find_dag(segment_runes);
        const auto mp_words = mp_words_from_dag(dict, dag);
        result.dags.push_back(typename BufferedMixResult<hmm>::SegmentDag{segment_pos, std::move(dag)});
        append_mix_words<hmm>(dict, model, result.words, mp_words, segment_runes, segment_pos);
    };

    cut_one(runes.subspan(pos, segments[0] - pos), pos);
    for (auto i = size_t{0}; i < segments.size() - 1; ++i) {
        result.words.push_back(WordRange{segments[i], static_cast<RuneIndex>(segments[i] + 1)});
        pos = segments[i] + 1;
        cut_one(runes.subspan(pos, segments[i + 1] - pos), pos);
    }

    return result;
}

template <bool hmm>
inline
auto query_cut_with_requery(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes)
    -> std::vector<WordRange> {
    auto mix_words = cut<MixSegment<hmm>>(dict, model, runes);
    auto result = std::vector<WordRange>{};
    result.reserve(mix_words.size() * 2);

    for (auto &word : mix_words) {
        append_sub_words_by_lookup<hmm>(dict, runes, word, result);
    }

    return result;
}

template <bool hmm>
inline
auto query_cut_with_buffered_dag(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes)
    -> std::vector<WordRange> {
    auto mix_result = mix_cut_with_dag_buffered<hmm>(dict, model, runes);
    auto result = std::vector<WordRange>{};
    result.reserve(mix_result.words.size() * 2);

    auto dag_index = size_t{0};
    for (auto &word : mix_result.words) {
        while (dag_index < mix_result.dags.size() && mix_result.dags[dag_index].end() <= word.begin) {
            ++dag_index;
        }

        const auto *segment = static_cast<const typename BufferedMixResult<hmm>::SegmentDag *>(nullptr);
        if (dag_index < mix_result.dags.size()) {
            auto &candidate = mix_result.dags[dag_index];
            if (candidate.offset <= word.begin && word.end <= candidate.end()) {
                segment = &candidate;
            }
        }

        if (segment) {
            append_sub_words_from_dag(segment->dag, segment->offset, word, result);
        } else {
            append_sub_words_by_lookup<hmm>(dict, runes, word, result);
        }
    }

    return result;
}

} // namespace detail

template <bool hmm = true>
inline
auto query_cut_requery(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes)
    -> std::vector<WordRange> {
    return detail::query_cut_with_requery<hmm>(dict, model, runes);
}

template <bool hmm = true>
inline
auto query_cut_buffered_dag(const DictTrie &dict, const HMModel &model, std::span<const Rune> runes)
    -> std::vector<WordRange> {
    return detail::query_cut_with_buffered_dag<hmm>(dict, model, runes);
}

} // namespace neo_cppjieba::test
