#pragma once

#include "neo/Config.hpp"
#include "neo/concept/Match.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/Dag.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/Trie.hpp"
#include "neo/third_party/gtl.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/StringUtil.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/Rune.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace neo_cppjieba::detail {

/// Bump when parsing, decoding or precedence rules change the dictionary text loading builds. Snapshots record
/// it because their content cannot be rederived without the source text.
inline constexpr auto kDictContentRules = std::uint32_t{1};

/// Main-dictionary frequency statistics fixed at load time; every weight derives from them.
struct FreqStats {
    std::int64_t sum{0};
    int min{0};
    int max{0};
    int median{0};

    auto operator==(const FreqStats &) const noexcept -> bool = default;
};

/// The unweighted value of a level-order edge; frequency zero selects the user default weight.
struct LevelOrderValue {
    int freq{0};
    PosTag tag{};

    auto operator==(const LevelOrderValue &) const noexcept -> bool = default;
};

/// A dictionary in the canonical layout snapshots store: the trie shape, unweighted values in edge order and the
/// statistics that weight them. Weights are derived on construction, so weight rules never need versioning.
struct LevelOrderDict {
    FreqStats stats;
    UserWordWeightOption user_word_weight_opt{};
    LevelOrderShape shape;
    std::vector<LevelOrderValue> values;
    std::vector<Rune> user_single_runes;

    auto operator==(const LevelOrderDict &) const -> bool = default;
};

// Describe the first violation of the invariants DictTrie relies on when building from a level-order dictionary,
// or return an empty view. Snapshot decoding reports it as invalid data.
[[nodiscard]] inline auto level_order_dict_error(const LevelOrderDict &dict) -> std::string_view {
    const auto &stats = dict.stats;
    if (stats.min <= 0 || stats.min > stats.median || stats.median > stats.max || stats.max > stats.sum) {
        return "frequency statistics are inconsistent";
    }
    if (const auto error = level_order_shape_error(dict.shape); !error.empty()) {
        return error;
    }
    if (!std::cmp_equal(std::ranges::count_if(dict.shape.edges, &LevelOrderEdge::has_value), dict.values.size())) {
        return "values do not match the valued edges";
    }
    if (std::ranges::any_of(dict.values, [](const LevelOrderValue &value) { return value.freq < 0; })) {
        return "a word frequency is negative";
    }
    if (!std::ranges::all_of(dict.user_single_runes, is_unicode_scalar)
        || std::ranges::adjacent_find(dict.user_single_runes, std::ranges::greater_equal{})
               != dict.user_single_runes.end()) {
        return "single-rune user words are not ascending Unicode scalars";
    }
    return {};
}

class DictTrie {
public:
    /// Construct from a main dictionary file and an optional user dictionary file.
    ///
    /// @param dict_path             path to the main dictionary (3-column: "word freq tag")
    /// @param user_dict_path        path to a single user dictionary file (empty = none)
    /// @param user_word_weight_opt  strategy for default weight of user words without freq
    explicit DictTrie(std::string_view dict_path, std::string_view user_dict_path,
                      UserWordWeightOption user_word_weight_opt) {
        check_user_word_weight_option(user_word_weight_opt);
        auto user_dicts = std::vector<DictSource>{};
        if (!user_dict_path.empty()) {
            user_dicts.push_back(DictSource::file(user_dict_path));
        }
        init(DictSource::file(dict_path), user_dicts, user_word_weight_opt);
    }

    /// Construct from file or memory sources. User dictionaries load in order; for a repeated word
    /// the last entry wins. Weight statistics come from the main dictionary only.
    explicit DictTrie(const DictSource &dict, std::span<const DictSource> user_dicts,
                      UserWordWeightOption user_word_weight_opt) {
        check_user_word_weight_option(user_word_weight_opt);
        init(dict, user_dicts, user_word_weight_opt);
    }

    /// Construct from a canonical level-order dictionary, such as a decoded snapshot, with the lookups and
    /// statistics text loading gives; weights are derived here under this build's rules.
    /// External data must pass level_order_dict_error first.
    explicit DictTrie(const LevelOrderDict &dict) {
        check_user_word_weight_option(dict.user_word_weight_opt);
        assert_check([&] { return level_order_dict_error(dict).empty(); }, "DictTrie: invalid level-order dictionary");
        apply_stats(dict.stats, dict.user_word_weight_opt);
        auto units = std::vector<DictUnit>{};
        units.reserve(dict.values.size());
        std::ranges::transform(dict.values, std::back_inserter(units), [this](const LevelOrderValue &value) {
            return DictUnit{value.freq > 0 ? freq_weight(value.freq) : user_word_default_weight_, value.tag};
        });
        trie_.build_level_order(dict.shape, units);
        user_dict_single_chinese_word_.insert(dict.user_single_runes.begin(), dict.user_single_runes.end());
    }

    /// Parse text sources with the rules construction applies, into the canonical layout snapshots store.
    /// Equal words keep their last entry, as construction does.
    [[nodiscard]] static auto level_order(const DictSource &dict, std::span<const DictSource> user_dicts,
                                          UserWordWeightOption user_word_weight_opt) -> LevelOrderDict {
        check_user_word_weight_option(user_word_weight_opt);
        auto parsed = parse_sources(dict, user_dicts);
        auto entries = sorted_last_entries(std::move(parsed.entries));
        auto keys = std::vector<Unicode>{};
        keys.reserve(entries.size());
        for (auto &entry : entries) {
            keys.push_back(std::move(entry.word));
        }
        auto layout = level_order_keys(keys);

        auto result = LevelOrderDict{};
        result.stats = parsed.stats;
        result.user_word_weight_opt = user_word_weight_opt;
        result.values.reserve(layout.value_keys.size());
        for (const auto key : layout.value_keys) {
            result.values.push_back(LevelOrderValue{entries[key].freq, entries[key].tag});
        }
        result.shape = std::move(layout.shape);
        result.user_single_runes.assign(parsed.user_single_runes.begin(), parsed.user_single_runes.end());
        std::ranges::sort(result.user_single_runes);
        return result;
    }

    DictTrie(DictTrie &&) noexcept = default;
    auto operator=(DictTrie &&) noexcept -> DictTrie & = default;
    auto operator=(const DictTrie &) -> DictTrie & = delete;

    /// Copy the dictionary, including runtime edits; copies are explicit because the trie is large.
    [[nodiscard]] auto clone() const -> DictTrie {
        return DictTrie{*this};
    }

    // ── Query interface ──────────────────────────────────────────────────

    /// Find exact match by rune span.
    [[nodiscard]] auto find(std::span<const Rune> key) const noexcept -> DictUnit {
        return trie_.find(key);
    }

    template <MatchVisitor Emit>
    auto for_each_match_from(std::span<const Rune> runes, RuneIndex begin, Emit &&emit) const
        noexcept(noexcept(trie_.for_each_match_from(runes, begin, std::forward<Emit>(emit)))) -> void {
        trie_.for_each_match_from(runes, begin, std::forward<Emit>(emit));
    }

    [[nodiscard]] auto find_dag(std::span<const Rune> runes) const -> Dag {
        return trie_.find_dag(runes);
    }

    // Reuse a caller-owned DAG without changing dictionary matching semantics.
    auto find_dag_into(std::span<const Rune> runes, Dag &dag) const -> void {
        trie_.find_dag_into(runes, dag);
    }

    /// Check if a single Chinese character comes from the user dictionary.
    /// MIX and SEARCH ask this for every single-rune route word, and most user dictionaries add none.
    [[nodiscard]] auto is_user_dict_single_chinese_word(Rune word) const noexcept -> bool {
        return !user_dict_single_chinese_word_.empty() && user_dict_single_chinese_word_.contains(word);
    }

    [[nodiscard]] auto min_weight() const noexcept -> detail::WordWeight {
        return min_weight_;
    }
    [[nodiscard]] auto max_weight() const noexcept -> detail::WordWeight {
        return max_weight_;
    }
    [[nodiscard]] auto median_weight() const noexcept -> detail::WordWeight {
        return median_weight_;
    }
    [[nodiscard]] auto user_word_default_weight() const noexcept -> detail::WordWeight {
        return user_word_default_weight_;
    }
    [[nodiscard]] auto freq_sum() const noexcept -> detail::WordWeight {
        return freq_sum_;
    }
    [[nodiscard]] auto trie() const noexcept -> const Trie & {
        return trie_;
    }

    // ── Runtime user words ───────────────────────────────────────────────
    //
    // Runtime words follow the user dictionary weight rules and keep the load-time frequency total.
    // Mutation needs exclusive access; callers choose locking, copy-on-write, or single-threaded use.

    /// Insert or replace a user word with the configured default user word weight.
    auto add_word(std::span<const Rune> word, PosTag tag) -> void {
        insert_user_word(word, DictUnit{user_word_default_weight_, tag});
    }

    /// Insert or replace a user word weighted by an explicit positive frequency.
    auto add_word_with_freq(std::span<const Rune> word, int freq, PosTag tag) -> void {
        check(freq > 0, "DictTrie: runtime user word frequency must be positive, got {}", freq);
        insert_user_word(word, DictUnit{freq_weight(freq), tag});
    }

    /// Remove a word whether it came from the main or a user dictionary; no earlier weight is restored.
    /// Returns false when the word is absent.
    auto remove_word(std::span<const Rune> word) -> bool {
        check(!word.empty(), "DictTrie: cannot remove an empty word");
        if (!trie_.erase(word)) {
            return false;
        }
        if (word.size() == 1) {
            user_dict_single_chinese_word_.erase(word.front());
        }
        return true;
    }

    /// Rebuild trie storage after many runtime edits without changing any lookup result.
    auto compact() -> void {
        trie_.compact();
    }

private:
    // ── Intermediate representation ──────────────────────────────────────
    //
    // Holds raw integer frequency during dictionary loading.
    // Discarded after the Trie is built — all final DictUnit values live
    // inside the Trie (no external std::vector / std::deque needed).

    struct RawEntry {
        Unicode word;
        int freq; // 0 ⇒ "use default weight" (user words without explicit freq)
        PosTag tag;
    };

    /// Text entries in load order, with the main-dictionary statistics and the single-rune user words.
    struct ParsedSources {
        std::vector<RawEntry> entries;
        FreqStats stats;
        gtl::flat_hash_set<Rune> user_single_runes;
    };

    // Sort entries by word; the stable sort keeps load order among equal words, so keeping the last of each run
    // applies the precedence Trie::build gives duplicates.
    [[nodiscard]] static auto sorted_last_entries(std::vector<RawEntry> entries) -> std::vector<RawEntry> {
        std::ranges::stable_sort(entries, std::ranges::less{}, &RawEntry::word);
        auto last = std::vector<RawEntry>{};
        last.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i + 1 == entries.size() || entries[i + 1].word != entries[i].word) {
                last.push_back(std::move(entries[i]));
            }
        }
        return last;
    }

    // Copying is reserved for clone(); Trie exposes its copy only through Trie::clone().
    DictTrie(const DictTrie &other)
        : trie_{other.trie_.clone()}, freq_sum_{other.freq_sum_}, min_weight_{other.min_weight_},
          max_weight_{other.max_weight_}, median_weight_{other.median_weight_},
          user_word_default_weight_{other.user_word_default_weight_},
          user_dict_single_chinese_word_{other.user_dict_single_chinese_word_} {
    }

    // Normalize by the main dictionary total fixed at load time, shared by loaded and runtime user words.
    [[nodiscard]] auto freq_weight(int f) const -> detail::WordWeight {
        assert_check([&] { return f > 0 && std::isfinite(freq_sum_) && freq_sum_ > 0.0; },
                     "DictTrie: invalid frequency normalization");
        if constexpr (compile_config::segmentation_style == SegmentationStyle::CPP) {
            return std::log(f / freq_sum_);
        } else {
            return std::log(static_cast<double>(f)) - std::log(freq_sum_);
        }
    }

    // Fix the weight statistics of the main dictionary; text and level-order construction share this step.
    auto apply_stats(const FreqStats &stats, UserWordWeightOption opt) -> void {
        freq_sum_ = static_cast<detail::WordWeight>(stats.sum);
        // Convert selected int frequencies to log-weights
        min_weight_ = freq_weight(stats.min);
        max_weight_ = freq_weight(stats.max);
        median_weight_ = freq_weight(stats.median);
        assert_check([&] { return min_weight_ <= median_weight_ && median_weight_ <= max_weight_; },
                     "DictTrie: inconsistent weight statistics");
        set_user_default_weight(opt);
    }

    // Public word arguments are validated here, so Trie::insert keeps only internal assertions.
    auto insert_user_word(std::span<const Rune> word, DictUnit value) -> void {
        check(!word.empty(), "DictTrie: cannot add an empty word");
        check(trie_.has_capacity_for(word), "DictTrie: adding a {}-rune word exceeds the configured trie node range",
              word.size());
        trie_.insert(word, value);
        if (word.size() == 1) {
            user_dict_single_chinese_word_.insert(word.front());
        }
    }

    // ── Core initialisation ──────────────────────────────────────────────
    //
    // Pipeline: load dicts → stats on int freqs → convert → build trie.
    // Statistics use integer frequencies (minmax_element, nth_element) in O(n);
    // log() is strictly monotonic, so they equal CppJieba's sorted-weight results.

    auto init(const DictSource &dict, std::span<const DictSource> user_dicts, UserWordWeightOption opt) -> void {
        auto parsed = parse_sources(dict, user_dicts);
        apply_stats(parsed.stats, opt);
        user_dict_single_chinese_word_ = std::move(parsed.user_single_runes);
        auto &entries = parsed.entries;

        // ── Phase 4: Build Trie from all entries ─────────────────────────
        //
        // Convert each RawEntry to (Unicode key, DictUnit value) and hand
        // them to Trie::build(). After this call the temporary vectors are
        // freed; the Trie owns all DictUnit data inline.

        auto keys = std::vector<Unicode>{};
        auto values = std::vector<DictUnit>{};
        keys.reserve(entries.size());
        values.reserve(entries.size());

        for (auto &e : entries) {
            auto w = e.freq > 0 ? freq_weight(e.freq) : user_word_default_weight_;
            keys.push_back(std::move(e.word));
            values.push_back(DictUnit{w, e.tag});
        }

        trie_.build(keys, values);
    }

    // Phases 1–3 of the pipeline, shared with level_order() so snapshots apply the same parsing rules.
    [[nodiscard]] static auto parse_sources(const DictSource &dict, std::span<const DictSource> user_dicts)
        -> ParsedSources {
        auto parsed = ParsedSources{};
        auto &entries = parsed.entries;

        // ── Phase 1: Load main dictionary ────────────────────────────────
        auto freqs = std::vector<int>{}; // parallel to entries (main dict only)
        detail::with_source_text(
            dict, [&](std::string_view text, std::string_view name) { load_main_dict(text, name, entries, freqs); });
        check(!entries.empty(), "DictTrie: main dictionary is empty: {}", dict.name());
        assert_check([&] { return entries.size() == freqs.size(); },
                     "DictTrie: main dictionary entries and frequencies must have equal sizes");

        // ── Phase 2: Compute statistics on integer frequencies ───────────
        auto freq_sum = int64_t{0};
        for (auto f : freqs) {
            assert_check([=] { return f > 0; }, "DictTrie: unchecked main dictionary frequency {}", f);
            check(f <= std::numeric_limits<int64_t>::max() - freq_sum,
                  "DictTrie: frequency sum exceeds the supported range: {}", dict.name());
            freq_sum += f;
        }
        assert_check([&] { return freq_sum > 0; }, "DictTrie: frequency sum must be positive");

        // min / max — O(n), single pass
        auto &&[min_it, max_it] = std::ranges::minmax_element(freqs);
        auto min_freq = *min_it;
        auto max_freq = *max_it;

        // median — O(n) average via nth_element (mutates freqs, but we
        // already extracted min/max values above)
        const auto median = freqs.begin() + static_cast<std::ptrdiff_t>(freqs.size() / 2);
        std::ranges::nth_element(freqs, median);
        auto median_freq = *median;
        parsed.stats = FreqStats{freq_sum, min_freq, max_freq, median_freq};

        // ── Phase 3: Load user dictionary entries ────────────────────────
        for (const auto &user_dict : user_dicts) {
            detail::with_source_text(user_dict, [&](std::string_view text, std::string_view) {
                load_user_dict(text, entries, parsed.user_single_runes);
            });
        }
        return parsed;
    }

    // ── Main dictionary loading ──────────────────────────────────────────
    //
    // Each line has exactly 3 space-separated fields: "word freq tag"
    // e.g.  "AT&T 3 nz",  "我们 � r"

    static auto load_main_dict(std::string_view text, std::string_view path, std::vector<RawEntry> &entries,
                               std::vector<int> &freqs) -> void {
        auto line = get_line_view(text);
        auto line_number = size_t{0};

        for (auto &&line_view : line) {
            ++line_number;
            if (line_view.empty()) {
                continue;
            }

            auto sv = get_split_view(line_view, ' ');
            auto it = sv.begin();
            const auto end = sv.end();

            assert_check([&] { return it != end; }, "DictTrie: a nonempty line must contain a field");
            auto word_sv = *it;
            ++it;
            check(it != end, "DictTrie: missing frequency at {}:{}", path, line_number);
            auto freq_sv = *it;
            ++it;
            check(it != end, "DictTrie: missing tag at {}:{}", path, line_number);
            auto tag_sv = *it;
            ++it;
            check(it == end, "DictTrie: expected exactly 3 fields at {}:{}", path, line_number);

            auto word = decode(word_sv);
            if (word.empty()) {
                continue;
            }

            auto freq = decode_value<int>(freq_sv);
            check(freq > 0, "DictTrie: frequency must be positive at {}:{}, got {}", path, line_number, freq);

            entries.push_back(RawEntry{std::move(word), freq, PosTag{tag_sv}});
            freqs.push_back(freq);
        }
    }

    /// Load the text of a single user dictionary.
    ///
    /// Supported line formats (fields separated by space):
    ///   1 field  — "word"            → default weight, empty tag
    ///   2 fields — "word tag"        → default weight, explicit tag
    ///   3 fields — "word freq tag"   → weight from freq, explicit tag
    static auto load_user_dict(std::string_view text, std::vector<RawEntry> &entries,
                               gtl::flat_hash_set<Rune> &single_runes) -> void {
        auto line = get_line_view(text);
        for (auto &&one_line : line) {
            if (one_line.empty()) {
                continue;
            }
            parse_user_dict_line(one_line, entries, single_runes);
        }
    }

    static auto parse_user_dict_line(std::string_view line, std::vector<RawEntry> &entries,
                                     gtl::flat_hash_set<Rune> &single_runes) -> void {
        auto sv = split_view{line, ' '};
        auto it = sv.begin();
        auto end = sv.end();

        if (it == end) {
            return;
        }

        auto word_sv = *it;
        ++it;
        auto word = decode(word_sv);
        if (word.empty()) {
            return;
        }

        auto freq = int{0}; // 0 → "use default weight"
        auto tag = PosTag{};

        if (it != end) {
            auto second = *it;
            ++it;
            if (it != end) {
                // 3 fields: word  freq  tag
                freq = decode_value<int>(second);
                check(freq >= 0, "DictTrie: user dictionary frequency must not be negative: '{}'", line);
                tag = PosTag{*it};
                ++it;
                check(it == end, "DictTrie: expected at most 3 user dictionary fields: '{}'", line);
            } else {
                // 2 fields: word  tag
                tag = PosTag{second};
            }
        }

        if (word.size() == 1) {
            single_runes.insert(word[0]);
        }

        entries.push_back(RawEntry{std::move(word), freq, tag});
    }

    auto set_user_default_weight(UserWordWeightOption opt) noexcept -> void {
        switch (opt) {
            case UserWordWeightOption::WordWeightMin: {
                user_word_default_weight_ = min_weight_;
                break;
            }
            case UserWordWeightOption::WordWeightMedian: {
                user_word_default_weight_ = median_weight_;
                break;
            }
            case UserWordWeightOption::WordWeightMax: {
                user_word_default_weight_ = max_weight_;
                break;
            }
            default: {
                assert_check([] { return false; }, "DictTrie: unchecked user word weight option {}",
                             static_cast<uint8_t>(opt));
                std::unreachable();
            }
        }
    }

    // ── Data members ─────────────────────────────────────────────────────
    // The Trie stores every DictUnit inline; no separate entry storage is kept.

    Trie trie_;
    detail::WordWeight freq_sum_{0.0};
    detail::WordWeight min_weight_{0.0};
    detail::WordWeight max_weight_{0.0};
    detail::WordWeight median_weight_{0.0};
    detail::WordWeight user_word_default_weight_{0.0};
    gtl::flat_hash_set<Rune> user_dict_single_chinese_word_;
};

} // namespace neo_cppjieba::detail
