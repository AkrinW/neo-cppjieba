/// Benchmark: pre_filter_view separator-lookup strategies
///
/// The production pre_filter_view uses a simple linear scan over
/// DEFAULT_SEPARATORS (5 runes). This benchmark compares that approach against
/// alternative lookup strategies to validate the design choice:
///
///   1. linear scan          (production — is_default_separator / pre_filter_view)
///   2. sorted array + binary search
///   3. std::unordered_set   (default load factor = 1.0)
///   4. std::unordered_set   (low load factor = 0.25)
///   5. std::unordered_set   (high load factor = 4.0)
///
/// All strategies iterate the same pre-decoded Chinese text file and produce
/// identical segment counts.

#include "neo/Utf.hpp"
#include "neo/algorithm/Separator.hpp"

#include "BenchmarkUtils.hpp"
#include "test_paths.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

struct BenchResult {
    const char *label;
    double total_ms;
    size_t rounds;
    size_t total_runes;
    uint64_t checksum;
};

static auto print_report(const BenchResult &r) -> void {
    auto total_runes_all = static_cast<double>(r.total_runes * r.rounds);
    auto ns_per_rune = r.total_ms * 1e6 / total_runes_all;
    auto runes_per_sec = total_runes_all / (r.total_ms / 1000.0);

    std::printf("│ %-40s │ %10.2f ms │ %8.2f ns/rune │ %8.2f Mr/s │\n", r.label, r.total_ms, ns_per_rune,
                runes_per_sec / 1e6);
}

// ─────────────────────────────────────────────────────────────────────────────
// Simulated workload for each segment type
// ─────────────────────────────────────────────────────────────────────────────
// on_separator:    lightweight operation for a single separator rune.
// on_text_segment: O(n) work proportional to segment length (simulates
//                  dictionary / DAG lookup).  Accepts empty spans (no-op).

struct SegmentStats {
    uint64_t checksum;
};

static inline auto on_separator(char32_t r, uint64_t &acc) -> void {
    acc ^= static_cast<uint64_t>(r);
}

static inline auto on_text_segment(std::span<const char32_t> seg, uint64_t &acc) -> void {
    for (auto r : seg) {
        acc = acc * 131 + static_cast<uint64_t>(r);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Load text file via mmap and decode to Unicode lines
// ─────────────────────────────────────────────────────────────────────────────
struct LineData {
    neo_cppjieba::detail::Unicode runes;
};

// The shared binary loader now also supports Windows; decoding remains outside the timed region.
static auto load_unicode_lines(const std::string &path) -> std::vector<LineData> {
    auto result = std::vector<LineData>{};

    for (const std::string_view line : load_lines(path)) {
        if (!line.empty()) {
            result.push_back(LineData{neo_cppjieba::Utf::decode(line)});
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Alternative separator-lookup strategies (benchmark-only, not used in production)
// ─────────────────────────────────────────────────────────────────────────────

// Concept for any type with `bool contains(char32_t) const`.
template <typename T>
concept SymbolSetLike = requires(char32_t r) {
    { T::contains(r) } -> std::convertible_to<bool>;
};

// hash_symbol_set: wraps std::unordered_set with configurable max_load_factor.
template <float load_factor>
class hash_symbol_set {
public:
    [[nodiscard]]
    static auto contains(char32_t r) -> bool {
        static const auto set = []() {
            const auto &symbols = neo_cppjieba::detail::DEFAULT_SEPARATORS;
            auto s = std::unordered_set<char32_t>{};
            s.max_load_factor(load_factor);
            s.reserve(symbols.size());
            s.insert(symbols.begin(), symbols.end());
            return s;
        }();
        return set.contains(r);
    }
};

// bitmask_symbol_set: uses a bitmask for runes <= 32 and direct comparison for CJK separators.
class bitmask_symbol_set {
public:
    [[nodiscard]]
    static constexpr auto contains(char32_t r) noexcept -> bool {
        return neo_cppjieba::detail::is_default_separator(r);
    }
};

class bitmask_symbol_set_branchless {
public:
    [[nodiscard]]
    static constexpr auto contains(char32_t r) noexcept -> bool {
        return neo_cppjieba::detail::is_default_separator_branchless(r);
    }
};

class bitmask_symbol_set_unlikely {
public:
    [[nodiscard]]
    static constexpr auto contains(char32_t r) noexcept -> bool {
        return neo_cppjieba::detail::is_default_separator_unlikely(r);
    }
};

// sorted_symbol_set: binary search on a pre-sorted span.
class sorted_symbol_set {
public:
    [[nodiscard]]
    static constexpr auto contains(char32_t r) noexcept -> bool {
        const auto &symbols = neo_cppjieba::detail::DEFAULT_SEPARATORS;

        auto lo = size_t{0};
        auto hi = symbols.size();

        while (lo < hi) {
            auto mid = lo + (hi - lo) / 2;
            if (symbols[mid] < r) {
                lo = mid + 1;
            } else if (symbols[mid] > r) {
                hi = mid;
            } else {
                return true;
            }
        }
        return false;
    }
};

class linear_symbol_set {
public:
    [[nodiscard]]
    static constexpr auto contains(char32_t r) noexcept -> bool {
        const auto &symbols = neo_cppjieba::detail::DEFAULT_SEPARATORS;
        for (auto s : symbols) {
            if (s == r) {
                return true;
            }
        }
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Generic pre_filter_view for alternative strategies (benchmark-only)
// ─────────────────────────────────────────────────────────────────────────────
template <SymbolSetLike SymbolSet>
class bench_pre_filter_view {
public:
    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::span<const char32_t>;

        iterator() noexcept = default;

        iterator(std::span<const char32_t> data) : data_(data), pos_(0), done_(false) {
            advance();
        }

        auto operator*() const noexcept -> std::span<const char32_t> {
            return current_;
        }
        auto operator++() -> iterator & {
            advance();
            return *this;
        }
        auto operator==(const iterator &other) const noexcept -> bool {
            return done_ == other.done_;
        }

    private:
        void advance() {
            if (pos_ >= data_.size()) {
                done_ = true;
                return;
            }
            auto begin = pos_;
            if (SymbolSet::contains(data_[pos_])) {
                ++pos_;
                current_ = data_.subspan(begin, 1);
            } else {
                while (pos_ < data_.size() && !SymbolSet::contains(data_[pos_])) {
                    ++pos_;
                }
                current_ = data_.subspan(begin, pos_ - begin);
            }
        }

        std::span<const char32_t> data_;
        size_t pos_ = 0;
        std::span<const char32_t> current_;
        bool done_ = true;
    };

    bench_pre_filter_view() noexcept = default;
    bench_pre_filter_view(std::span<const char32_t> data) noexcept : data_(data) {
    }

    [[nodiscard]]
    auto begin() const -> iterator {
        return iterator{data_};
    }
    [[nodiscard]]
    auto end() const noexcept -> iterator {
        return iterator{};
    }

private:
    std::span<const char32_t> data_;
};

template <SymbolSetLike SymbolSet>
bench_pre_filter_view(std::span<const char32_t>, const SymbolSet &) -> bench_pre_filter_view<SymbolSet>;

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark drivers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Segment counting helpers for correctness verification
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Index pre-scan ("vectorized") strategy
// ─────────────────────────────────────────────────────────────────────────────
// Instead of using an iterator that interleaves separator detection with segment
// construction, this approach separates the work into two passes:
//   Pass 1: scan the entire rune array and collect indices of all separators.
//   Pass 2: iterate the indices to emit segments (non-separator spans + separator spans).
// Pass 1 is a simple branchless-friendly loop that the compiler may auto-vectorize.
static auto count_segments_production(const std::vector<LineData> &lines) -> SegmentStats {
    auto checksum = uint64_t{0};
    for (auto &&line : lines) {
        auto runes = std::span<const char32_t>{line.runes};
        if (runes.empty()) {
            continue;
        }
        auto seps = neo_cppjieba::detail::get_pre_filter_separators(runes);
        auto pos = uint32_t{0};
        on_text_segment(runes.subspan(pos, seps[0] - pos), checksum);
        for (auto i = size_t{0}; i < seps.size() - 1; ++i) {
            on_separator(runes[seps[i]], checksum);
            pos = seps[i] + 1;
            on_text_segment(runes.subspan(pos, seps[i + 1] - pos), checksum);
        }
    }
    return SegmentStats{checksum};
}

template <SymbolSetLike SymbolSet>
static auto count_segments_alt(const std::vector<LineData> &lines) -> SegmentStats {
    auto checksum = uint64_t{0};
    for (auto &&line : lines) {
        for (auto seg : bench_pre_filter_view<SymbolSet>{std::span<const char32_t>{line.runes}}) {
            if (seg.size() == 1 && SymbolSet::contains(seg[0])) {
                on_separator(seg[0], checksum);
            } else {
                on_text_segment(seg, checksum);
            }
        }
    }
    return SegmentStats{checksum};
}

// ── Strategy: bitmask + bool mask (iterator-style) ──────────────────────────
// Pass 1: fill bool mask (vectorizable).
// Pass 2: iterate runes using the mask to emit segments.
template <SymbolSetLike SymbolSet>
static auto count_segments_mask_iter(const std::vector<LineData> &lines) -> SegmentStats {
    auto checksum = uint64_t{0};
    auto mask = std::vector<uint8_t>{};
    for (auto &&line : lines) {
        auto runes = std::span<const char32_t>{line.runes};
        auto n = runes.size();
        mask.resize(n);
        for (auto i = size_t{0}; i < n; ++i) {
            mask[i] = SymbolSet::contains(runes[i]);
        }
        auto i = size_t{0};
        while (i < n) {
            if (mask[i]) {
                on_separator(runes[i], checksum);
                ++i;
            } else {
                auto begin = i;
                while (i < n && !mask[i]) {
                    ++i;
                }
                on_text_segment(runes.subspan(begin, i - begin), checksum);
            }
        }
    }
    return SegmentStats{checksum};
}

// ── Strategy: index pre-scan + bool mask ────────────────────────────────────
// Pass 1: fill bool mask (vectorizable).
// Pass 1.5: extract separator indices from mask.
// Pass 2: emit segments from indices.
template <SymbolSetLike SymbolSet>
static auto count_segments_mask_index(const std::vector<LineData> &lines) -> SegmentStats {
    auto checksum = uint64_t{0};
    auto mask = std::vector<uint8_t>{};
    auto sep_indices = std::vector<size_t>{};
    for (auto &&line : lines) {
        auto runes = std::span<const char32_t>{line.runes};
        auto n = runes.size();
        mask.resize(n);
        for (auto i = size_t{0}; i < n; ++i) {
            mask[i] = SymbolSet::contains(runes[i]);
        }
        sep_indices.clear();
        for (auto i = size_t{0}; i < n; ++i) {
            if (mask[i]) {
                sep_indices.push_back(i);
            }
        }
        auto pos = size_t{0};
        for (auto idx : sep_indices) {
            if (idx > pos) {
                on_text_segment(runes.subspan(pos, idx - pos), checksum);
            }
            on_separator(runes[idx], checksum);
            pos = idx + 1;
        }
        if (pos < n) {
            on_text_segment(runes.subspan(pos, n - pos), checksum);
        }
    }
    return SegmentStats{checksum};
}

// Benchmark an alternative strategy using bench_pre_filter_view<SymbolSet>.
template <SymbolSetLike SymbolSet>
static auto bench_alternative(const char *label, const std::vector<LineData> &lines, size_t total_runes, size_t rounds)
    -> BenchResult {
    // Warm up
    {
        auto checksum = uint64_t{0};
        for (auto &&line : lines) {
            for (auto seg : bench_pre_filter_view<SymbolSet>{std::span<const char32_t>{line.runes}}) {
                if (seg.size() == 1 && SymbolSet::contains(seg[0])) {
                    on_separator(seg[0], checksum);
                } else {
                    on_text_segment(seg, checksum);
                }
            }
        }
        DoNotOptimize(checksum);
    }

    auto checksum = uint64_t{0};
    auto t0 = Clock::now();
    for (auto round = size_t{0}; round < rounds; ++round) {
        for (auto &&line : lines) {
            for (auto seg : bench_pre_filter_view<SymbolSet>{std::span<const char32_t>{line.runes}}) {
                if (seg.size() == 1 && SymbolSet::contains(seg[0])) {
                    on_separator(seg[0], checksum);
                } else {
                    on_text_segment(seg, checksum);
                }
            }
        }
    }
    auto t1 = Clock::now();
    DoNotOptimize(checksum);

    return BenchResult{label, Ms(t1 - t0).count(), rounds, total_runes, checksum};
}

static auto bench_index_scan(const char *label, const std::vector<LineData> &lines, size_t total_runes, size_t rounds)
    -> BenchResult {
    // Warm up
    auto seps = std::vector<uint32_t>{};
    {
        auto checksum = uint64_t{0};
        for (auto &&line : lines) {
            auto runes = std::span<const char32_t>{line.runes};
            if (runes.empty()) {
                continue;
            }
            // auto seps = neo_cppjieba::detail::get_pre_filter_separators(runes);
            neo_cppjieba::detail::get_pre_filter_separators(runes, seps);
            auto pos = size_t{0};
            on_text_segment(runes.subspan(pos, seps[0] - pos), checksum);
            for (auto i = size_t{0}; i < seps.size() - 1; ++i) {
                on_separator(runes[seps[i]], checksum);
                pos = seps[i] + 1;
                on_text_segment(runes.subspan(pos, seps[i + 1] - pos), checksum);
            }
        }
        DoNotOptimize(checksum);
    }

    auto checksum = uint64_t{0};
    auto t0 = Clock::now();
    for (auto round = size_t{0}; round < rounds; ++round) {
        for (auto &&line : lines) {
            auto runes = std::span<const char32_t>{line.runes};
            if (runes.empty()) {
                continue;
            }
            // auto sep_indices = neo_cppjieba::detail::get_pre_filter_separators(runes);
            neo_cppjieba::detail::get_pre_filter_separators(runes, seps);
            auto pos = size_t{0};
            on_text_segment(runes.subspan(pos, seps[0] - pos), checksum);
            for (auto i = size_t{0}; i < seps.size() - 1; ++i) {
                on_separator(runes[seps[i]], checksum);
                pos = seps[i] + 1;
                on_text_segment(runes.subspan(pos, seps[i + 1] - pos), checksum);
            }
        }
    }
    auto t1 = Clock::now();
    DoNotOptimize(checksum);

    return BenchResult{label, Ms(t1 - t0).count(), rounds, total_runes, checksum};
}

// ─────────────────────────────────────────────────────────────────────────────
// Bool-mask strategies: generate a flat bool array first (vectorizable pass),
// then derive segments from the mask. The mask-generation loop has no branches
// or side-effects, allowing the compiler to auto-vectorize with AVX2.
// ─────────────────────────────────────────────────────────────────────────────

template <SymbolSetLike SymbolSet>
static auto bench_mask_iter(const char *label, const std::vector<LineData> &lines, size_t total_runes, size_t rounds)
    -> BenchResult {
    auto mask = std::vector<uint8_t>{};
    // Warm up
    {
        auto checksum = uint64_t{0};
        for (auto &&line : lines) {
            auto runes = std::span<const char32_t>{line.runes};
            auto n = runes.size();
            mask.resize(n);
            for (auto i = size_t{0}; i < n; ++i) {
                mask[i] = SymbolSet::contains(runes[i]);
            }
            auto i = size_t{0};
            while (i < n) {
                if (mask[i]) {
                    on_separator(runes[i], checksum);
                    ++i;
                } else {
                    auto begin = i;
                    while (i < n && !mask[i]) {
                        ++i;
                    }
                    on_text_segment(runes.subspan(begin, i - begin), checksum);
                }
            }
        }
        DoNotOptimize(checksum);
    }

    auto checksum = uint64_t{0};
    auto t0 = Clock::now();
    for (auto round = size_t{0}; round < rounds; ++round) {
        for (auto &&line : lines) {
            auto runes = std::span<const char32_t>{line.runes};
            auto n = runes.size();
            mask.resize(n);
            // Pass 1: fill mask (vectorizable, no branches)
            for (auto i = size_t{0}; i < n; ++i) {
                mask[i] = SymbolSet::contains(runes[i]);
            }
            // Pass 2: emit segments from mask
            auto i = size_t{0};
            while (i < n) {
                if (mask[i]) {
                    on_separator(runes[i], checksum);
                    ++i;
                } else {
                    auto begin = i;
                    while (i < n && !mask[i]) {
                        ++i;
                    }
                    on_text_segment(runes.subspan(begin, i - begin), checksum);
                }
            }
        }
    }
    auto t1 = Clock::now();
    DoNotOptimize(checksum);
    return BenchResult{label, Ms(t1 - t0).count(), rounds, total_runes, checksum};
}

template <SymbolSetLike SymbolSet>
static auto bench_mask_index(const char *label, const std::vector<LineData> &lines, size_t total_runes, size_t rounds)
    -> BenchResult {
    auto mask = std::vector<uint8_t>{};
    auto sep_indices = std::vector<size_t>{};
    // Warm up
    {
        auto checksum = uint64_t{0};
        for (auto &&line : lines) {
            auto runes = std::span<const char32_t>{line.runes};
            auto n = runes.size();
            mask.resize(n);
            for (auto i = size_t{0}; i < n; ++i) {
                mask[i] = SymbolSet::contains(runes[i]);
            }
            sep_indices.clear();
            for (auto i = size_t{0}; i < n; ++i) {
                if (mask[i]) {
                    sep_indices.push_back(i);
                }
            }
            auto pos = size_t{0};
            for (auto idx : sep_indices) {
                if (idx > pos) {
                    on_text_segment(runes.subspan(pos, idx - pos), checksum);
                }
                on_separator(runes[idx], checksum);
                pos = idx + 1;
            }
            if (pos < n) {
                on_text_segment(runes.subspan(pos, n - pos), checksum);
            }
        }
        DoNotOptimize(checksum);
    }

    auto checksum = uint64_t{0};
    auto t0 = Clock::now();
    for (auto round = size_t{0}; round < rounds; ++round) {
        for (auto &&line : lines) {
            auto runes = std::span<const char32_t>{line.runes};
            auto n = runes.size();
            mask.resize(n);
            // Pass 1: fill mask (vectorizable)
            for (auto i = size_t{0}; i < n; ++i) {
                mask[i] = SymbolSet::contains(runes[i]);
            }
            // Pass 1.5: extract separator indices from mask
            sep_indices.clear();
            for (auto i = size_t{0}; i < n; ++i) {
                if (mask[i]) {
                    sep_indices.push_back(i);
                }
            }
            // Pass 2: emit segments from indices
            auto pos = size_t{0};
            for (auto idx : sep_indices) {
                if (idx > pos) {
                    on_text_segment(runes.subspan(pos, idx - pos), checksum);
                }
                on_separator(runes[idx], checksum);
                pos = idx + 1;
            }
            if (pos < runes.size()) {
                on_text_segment(runes.subspan(pos, runes.size() - pos), checksum);
            }
        }
    }
    auto t1 = Clock::now();
    DoNotOptimize(checksum);
    return BenchResult{label, Ms(t1 - t0).count(), rounds, total_runes, checksum};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
auto main(int, char *[]) -> int {
    namespace neo = neo_cppjieba;

    auto text_path = std::string(TEST_DATA_DIR) + "/weicheng.utf8";

    constexpr auto ROUNDS = size_t{500};

    std::printf("Text file  : %s\n", text_path.c_str());
    std::printf("Rounds     : %zu\n", ROUNDS);
    std::printf("Separators : %zu runes (tab, newline, space, 。, ，)\n\n", neo::detail::DEFAULT_SEPARATORS.size());

    // ── Load and decode ──────────────────────────────────────────────────
    auto lines = load_unicode_lines(text_path);
    auto total_runes = size_t{0};
    for (auto &&l : lines) {
        total_runes += l.runes.size();
    }
    std::printf("Loaded %zu lines (%zu runes total)\n\n", lines.size(), total_runes);

    // ── Verify correctness ───────────────────────────────────────────────
    auto ref = count_segments_production(lines);
    std::printf("Verifying all strategies produce identical segment counts and checksums ...\n");

    auto ok = true;
    auto check = [&](const char *name, SegmentStats got) {
        if (got.checksum != ref.checksum) {
            std::printf("  ✗ %s: checksum %llu (exp %llu)\n", name, static_cast<unsigned long long>(got.checksum),
                        static_cast<unsigned long long>(ref.checksum));
            ok = false;
        }
    };
    check("linear", count_segments_alt<linear_symbol_set>(lines));
    check("sorted (binary)", count_segments_alt<sorted_symbol_set>(lines));
    check("hash (lf=1.0)", count_segments_alt<hash_symbol_set<1.0f>>(lines));
    check("hash (lf=0.25)", count_segments_alt<hash_symbol_set<0.25f>>(lines));
    check("hash (lf=4.0)", count_segments_alt<hash_symbol_set<4.0f>>(lines));
    check("bitmask", count_segments_alt<bitmask_symbol_set>(lines));
    check("bitmask_branchless", count_segments_alt<bitmask_symbol_set_branchless>(lines));
    check("bitmask_unlikely", count_segments_alt<bitmask_symbol_set_unlikely>(lines));
    check("index pre-scan", count_segments_production(lines));
    check("mask + iter", count_segments_mask_iter<bitmask_symbol_set_unlikely>(lines));
    check("mask + index", count_segments_mask_index<bitmask_symbol_set_unlikely>(lines));

    if (!ok) {
        std::printf("  ✗ Segment counts or checksums differ — aborting.\n");
        return 1;
    }
    std::printf("  ✓ All strategies: checksum %llu.\n\n", static_cast<unsigned long long>(ref.checksum));

    // ── Benchmark ────────────────────────────────────────────────────────
    std::printf("┌──────────────────────────────────────────┬───────────────┬─────────────────┬──────────────┐\n");
    std::printf("│ %-40s │ %-13s │ %-15s │ %-12s │\n", "Strategy", "Total time", "Latency", "Throughput");
    std::printf("├──────────────────────────────────────────┼───────────────┼─────────────────┼──────────────┤\n");

    auto r1 = bench_alternative<linear_symbol_set>("linear scan (production)", lines, total_runes, ROUNDS);
    print_report(r1);

    auto r2 = bench_alternative<sorted_symbol_set>("sorted array + binary search", lines, total_runes, ROUNDS);
    print_report(r2);

    auto r3 = bench_alternative<hash_symbol_set<1.0f>>("unordered_set (lf=1.0, default)", lines, total_runes, ROUNDS);
    print_report(r3);

    auto r4 = bench_alternative<hash_symbol_set<0.25f>>("unordered_set (lf=0.25, sparse)", lines, total_runes, ROUNDS);
    print_report(r4);

    auto r5 = bench_alternative<hash_symbol_set<4.0f>>("unordered_set (lf=4.0, dense)", lines, total_runes, ROUNDS);
    print_report(r5);

    auto r6 = bench_alternative<bitmask_symbol_set>("bitmask", lines, total_runes, ROUNDS);
    print_report(r6);

    auto r7 = bench_alternative<bitmask_symbol_set_branchless>("bitmask (branchless)", lines, total_runes, ROUNDS);
    print_report(r7);

    auto r8 = bench_alternative<bitmask_symbol_set_unlikely>("bitmask (unlikely)", lines, total_runes, ROUNDS);
    print_report(r8);

    auto r9 = bench_index_scan("index pre-scan (vectorizable)", lines, total_runes, ROUNDS);
    print_report(r9);

    auto r10 = bench_mask_iter<bitmask_symbol_set_unlikely>("bool-mask + iterator", lines, total_runes, ROUNDS);
    print_report(r10);

    auto r11 = bench_mask_index<bitmask_symbol_set_unlikely>("bool-mask + index extract", lines, total_runes, ROUNDS);
    print_report(r11);

    std::printf("└──────────────────────────────────────────┴───────────────┴─────────────────┴──────────────┘\n\n");

    // ── Summary ──────────────────────────────────────────────────────────
    auto results = std::array<BenchResult, 11>{r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11};
    auto *fastest = &results[0];
    for (auto &r : results) {
        if (r.total_ms < fastest->total_ms) {
            fastest = &r;
        }
    }

    std::printf("══════════════════════════════════════════════════════════\n");
    std::printf("  FASTEST: %s\n", fastest->label);
    std::printf("══════════════════════════════════════════════════════════\n");
    std::printf("  Speedup vs others:\n");
    for (auto &r : results) {
        if (&r == fastest) {
            continue;
        }
        std::printf("    vs %-36s : %.2fx\n", r.label, r.total_ms / fastest->total_ms);
    }
    std::printf("══════════════════════════════════════════════════════════\n");

    return 0;
}
