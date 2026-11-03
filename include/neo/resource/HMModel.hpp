#pragma once

#include "neo/resource/HMMState.hpp"
#include "neo/third_party/gtl.hpp"
#include "neo/util/ForceInline.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/StringUtil.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/Rune.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neo_cppjieba::detail {

/// Emit probability map: maps a single Unicode code point (Rune) to its log-probability.
using EmitProbMap = std::unordered_map<Rune, double>;

inline constexpr auto MIN_DOUBLE = -3.14e+100;
inline constexpr auto MAX_DOUBLE = 3.14e+100;

/// HMModel loads a Hidden Markov Model for Chinese word segmentation.
///
/// The model contains four hidden states — B (Begin), E (End), M (Middle), S (Single) —
/// and three sets of parameters:
///   - start probabilities   (initial state distribution)
///   - transition probabilities (state-to-state matrix)
///   - emission probabilities  (state → observed character)
///
/// File format (dict/hmm_model.utf8):
///   - Lines starting with '#' and blank lines are skipped.
///   - Line 1:  start probabilities (4 space-separated doubles)
///   - Lines 2–5: transition probability matrix (4 × 4, space-separated)
///   - Lines 6–9: emission probabilities for B, E, M, S respectively
///               (comma-separated "char:prob" pairs, e.g. "我:-4.711846,的:-6.412328")
class HMModel {
public:
    // ── Constructors ─────────────────────────────────────────────────────
    /// Construct and load the model from the given file path.
    explicit HMModel(std::string_view model_path) : HMModel(DictSource::file(model_path)) {
    }

    /// Construct and load the model from a file or memory source.
    explicit HMModel(const DictSource &source) {
        detail::with_source_text(source, [this](std::string_view text, std::string_view name) { load(text, name); });
    }

    HMModel(HMModel &&) noexcept = default;
    auto operator=(HMModel &&) noexcept -> HMModel & = default;
    auto operator=(const HMModel &) -> HMModel & = delete;

    // Copying the emission tables stays explicit at call sites.
    [[nodiscard]] auto clone() const -> HMModel {
        return HMModel{*this};
    }

    [[nodiscard]] constexpr auto get_start_prob(HMMState state) const noexcept -> double {
        assert_check([=] { return static_cast<size_t>(state) < kHMMStatesNum; }, "HMModel: invalid start state");
        return start_prob[static_cast<size_t>(state)];
    }

    [[nodiscard]] constexpr auto get_trans_prob(HMMState from, HMMState to) const noexcept -> double {
        assert_check([=] { return static_cast<size_t>(from) < kHMMStatesNum; },
                     "HMModel: invalid transition source state");
        assert_check([=] { return static_cast<size_t>(to) < kHMMStatesNum; },
                     "HMModel: invalid transition target state");
        return trans_prob[static_cast<size_t>(from)][static_cast<size_t>(to)];
    }

    // ── Query interface ──────────────────────────────────────────────────

    /// Look up one state's emission probability of `key`; absent entries return MIN_DOUBLE.
    [[nodiscard]] auto get_emit_prob(HMMState state, Rune key) const noexcept -> double {
        assert_check([=] { return static_cast<size_t>(state) < kHMMStatesNum; }, "HMModel: invalid emission state");
        return get_emit_probs(key)[static_cast<size_t>(state)];
    }

    /// Fetch all four states with one rune lookup; absent states keep MIN_DOUBLE.
    [[nodiscard, NEO_FORCE_INLINE]]
    auto get_emit_probs(Rune key) const noexcept -> EmitProbabilities {
        // Unsigned subtraction rejects runes on either side of the stored BMP interval.
        const auto index = static_cast<size_t>(key) - bmp_begin_;
        if (index < bmp_emit_probs_.size()) {
            return bmp_emit_probs_[index];
        }
        return get_sparse_emit_probs(key);
    }

private:
    HMModel(const HMModel &) = default;

    // Viterbi calls get_emit_probs once per rune, so it is forced inline; the hash lookup for runes outside the dense
    // interval stays out of it, keeping that rare path from being copied into every caller.
    [[nodiscard]] auto get_sparse_emit_probs(Rune key) const noexcept -> EmitProbabilities {
        const auto it = supplementary_emit_probs_.find(key);
        return it != supplementary_emit_probs_.end() ? it->second : kMissingEmitProbs;
    }

    // ── Model parameters (inspect through get_start_prob and get_trans_prob) ─────────

    std::array<double, kHMMStatesNum> start_prob{};
    std::array<std::array<double, kHMMStatesNum>, kHMMStatesNum> trans_prob{};

    using EmitTable = gtl::flat_hash_map<Rune, EmitProbabilities>;
    static constexpr auto kMissingEmitProbs = EmitProbabilities{MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE};
    static constexpr auto kBmpLimit = size_t{0x10000};
    size_t bmp_begin_{0};
    std::vector<EmitProbabilities> bmp_emit_probs_;
    EmitTable supplementary_emit_probs_;

    /// Build rune rows only after the original per-state parsing and validation succeed.
    auto build_emit_probs(const std::array<EmitProbMap, kHMMStatesNum> &maps) -> void {
        size_t bmp_begin = kBmpLimit;
        size_t bmp_end = 0;
        size_t supplementary_capacity = 0;
        for (const auto &map : maps) {
            size_t supplementary_count = 0;
            for (const auto &[rune, probability] : map) {
                if (rune < kBmpLimit) {
                    bmp_begin = std::min(bmp_begin, static_cast<size_t>(rune));
                    bmp_end = std::max(bmp_end, static_cast<size_t>(rune) + 1);
                } else {
                    ++supplementary_count;
                }
            }
            supplementary_capacity = std::max(supplementary_capacity, supplementary_count);
        }
        if (bmp_end > bmp_begin) {
            bmp_begin_ = bmp_begin;
            bmp_emit_probs_.resize(bmp_end - bmp_begin, kMissingEmitProbs);
        }
        supplementary_emit_probs_.reserve(supplementary_capacity);
        for (size_t state = 0; state < kHMMStatesNum; ++state) {
            for (const auto &[rune, probability] : maps[state]) {
                if (rune < kBmpLimit) {
                    const auto index = static_cast<size_t>(rune) - bmp_begin_;
                    assert_check([&] { return index < bmp_emit_probs_.size(); },
                                 "HMModel: BMP emission index is out of range");
                    bmp_emit_probs_[index][state] = probability;
                } else {
                    auto &row = supplementary_emit_probs_.try_emplace(rune, kMissingEmitProbs).first->second;
                    row[state] = probability;
                }
            }
        }
    }

    // ── Loading ──────────────────────────────────────────────────────────
    auto load(std::string_view text, std::string_view model_path) -> void {
        auto lines = get_line_view(text);
        constexpr auto expected_line_count = 1 + 2 * kHMMStatesNum;

        // Collect non-comment, non-empty lines.
        auto data_lines = std::vector<std::string_view>{};
        // 4 (startProb) + 4 (transProb rows) + 4 (emitProb) = up to ~12 data lines, but
        // startProb is 1 line, so total 9 data lines minimum.
        data_lines.reserve(expected_line_count);

        for (auto &&line : lines) {
            auto trimmed = trim(line);
            if (trimmed.empty() || trimmed.front() == '#') {
                continue;
            }
            check(data_lines.size() < expected_line_count,
                  "HMModel: expected exactly {} data lines in {}, got extra data", expected_line_count, model_path);
            data_lines.push_back(trimmed);
        }

        // We expect at least 9 data lines:
        //   1 (startProb) + 4 (transProb) + 4 (emitProb B/E/M/S)
        // Exactly these 9 data lines are accepted; additional data lines are rejected.
        check(data_lines.size() == expected_line_count, "HMModel: expected exactly {} data lines in {}, got {}",
              expected_line_count, model_path, data_lines.size());

        auto emit_prob_maps = std::array<EmitProbMap, kHMMStatesNum>{};
        auto idx = size_t{0};

        // ── Start probabilities ──────────────────────────────────────────
        start_prob = parse_prob_line(data_lines[idx]);
        ++idx;

        // ── Transition probabilities ─────────────────────────────────────
        for (auto i = size_t{0}; i < kHMMStatesNum; ++i) {
            trans_prob[i] = parse_prob_line(data_lines[idx]);
            ++idx;
        }

        // ── Emission probabilities ───────────────────────────────────────
        for (auto i = size_t{0}; i < kHMMStatesNum; ++i) {
            emit_prob_maps[i] = parse_emit_prob_map(data_lines[idx]);
            ++idx;
        }
        build_emit_probs(emit_prob_maps);
    }

    // Both probability formats use finite log-weights bounded by the Viterbi sentinel and log(1).
    static auto parse_log_prob(std::string_view token) -> double {
        const auto probability = decode_value<double>(token);
        check(std::isfinite(probability) && probability >= MIN_DOUBLE && probability <= 0.0,
              "HMModel: log-probability must be finite and in [{}, 0], got '{}'", MIN_DOUBLE, token);
        return probability;
    }

    /// Parse a space-separated line of doubles into a fixed-size array.
    auto parse_prob_line(std::string_view line) -> std::array<double, kHMMStatesNum> {
        auto out = std::array<double, kHMMStatesNum>{};
        auto i = size_t{0};
        auto split = get_split_view(line, ' ');
        for (auto &&token : split) {
            if (token.empty()) {
                continue;
            }
            check(i < kHMMStatesNum, "HMModel: too many values in probability line");
            out[i] = parse_log_prob(token);
            ++i;
        }
        check(i == kHMMStatesNum, "HMModel: expected {} values in probability line, got {}", kHMMStatesNum, i);
        return out;
    }

    /// Parse an emit-probability line: comma-separated "char:prob" pairs.
    auto parse_emit_prob_map(std::string_view line) -> EmitProbMap {
        check(!line.empty(), "HMModel: empty emit probability line");
        auto mp = EmitProbMap{};
        auto split = get_split_view(line, ',');
        for (auto &&pair : split) {
            if (pair.empty()) {
                continue;
            }
            auto colon_pos = pair.find(':');
            check(colon_pos != std::string_view::npos, "HMModel: malformed emit prob entry (no ':'): {}", pair);

            auto char_sv = pair.substr(0, colon_pos);
            auto prob_sv = pair.substr(colon_pos + 1);

            auto unicode = decode(char_sv);
            check(unicode.size() == 1, "HMModel: emit prob key must be a single character, got '{}' ({} runes)",
                  char_sv, unicode.size());

            const auto inserted = mp.emplace(unicode[0], parse_log_prob(prob_sv)).second;
            check(inserted, "HMModel: duplicate emit probability key '{}'", char_sv);
        }
        check(!mp.empty(), "HMModel: emission probability map must contain at least one entry");
        mp.reserve(mp.size() * 4); // reserve space to increase query performance by avoiding rehashing.
        return mp;
    }
}; // class HMModel

} // namespace neo_cppjieba::detail
