#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/resource/Dag.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/Trie.hpp"
#include "neo/resource/TrieStats.hpp"
#include "neo/util/FileIO.hpp"
#include "neo/util/StringUtil.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"

#include "test_paths.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <map>
#include <print>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

TEST(DictUnitTest, DefaultValueIsMissing) {
    EXPECT_FALSE(DictUnit{}.has_value());
}

TEST(DictUnitTest, SignedZeroWeightsArePresent) {
    for (const auto weight : {0.0f, -0.0f}) {
        const auto value = DictUnit{weight, PosTag{}};
        EXPECT_TRUE(value.has_value());
    }
}

TEST(TrieTest, FindsZeroWeightWordWithoutTreatingItsPrefixAsAWord) {
    const auto keys = std::vector<Unicode>{decode("𠮷中")};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{}}};
    auto trie = Trie{};
    trie.build(keys, values);

    const auto found = trie.find(decode("𠮷中"));
    ASSERT_TRUE(found.has_value());
    EXPECT_FLOAT_EQ(found.weight, 0.0f);
    EXPECT_FALSE(trie.find(decode("𠮷")).has_value());
    EXPECT_FALSE(trie.find(decode("")).has_value());
    EXPECT_FALSE(trie.find(decode("外")).has_value());
}

TEST(TrieTest, DagDistinguishesZeroWeightWordsFromUnknownRunes) {
    const auto keys = std::vector<Unicode>{decode("𠮷"), decode("𠮷中")};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{}}, {0.0f, PosTag{}}};
    auto trie = Trie{};
    trie.build(keys, values);

    const auto dag = trie.find_dag(decode("𠮷中外"));
    const auto matches = dag.get_edges(0);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].next_pos, 1u);
    EXPECT_FLOAT_EQ(matches[0].weight, 0.0f);
    EXPECT_EQ(matches[1].next_pos, 2u);
    EXPECT_FLOAT_EQ(matches[1].weight, 0.0f);
    const auto unknown = dag.get_edges(2);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0].weight, kMissingWordWeight);
}

TEST(TrieTest, LastDuplicateValueAndTagWin) {
    const auto keys = std::vector<Unicode>{decode("词条"), decode("词条")};
    const auto values = std::vector<DictUnit>{{-1.0f, PosTag{"n"}}, {-2.0f, PosTag{"v"}}};
    auto trie = Trie{};
    trie.build(keys, values);

    const auto found = trie.find(decode("词条"));
    ASSERT_TRUE(found.has_value());
    EXPECT_FLOAT_EQ(found.weight, -2.0f);
    EXPECT_EQ(found.tag, PosTag{"v"});
    EXPECT_EQ(trie.collect_stats().value_count, 1u);
}

TEST(TrieTest, DuplicatePrefixCanBecomeZeroWeightWithoutLosingDescendants) {
    const auto keys = std::vector<Unicode>{decode("词"), decode("词条"), decode("词")};
    const auto values = std::vector<DictUnit>{{-1.0f, PosTag{"n"}}, {-2.0f, PosTag{"n"}}, {0.0f, PosTag{"v"}}};
    auto trie = Trie{};
    trie.build(keys, values);

    const auto prefix = trie.find(decode("词"));
    ASSERT_TRUE(prefix.has_value());
    EXPECT_FLOAT_EQ(prefix.weight, 0.0f);
    EXPECT_EQ(prefix.tag, PosTag{"v"});
    const auto descendant = trie.find(decode("词条"));
    ASSERT_TRUE(descendant.has_value());
    EXPECT_FLOAT_EQ(descendant.weight, -2.0f);
    EXPECT_EQ(descendant.tag, PosTag{"n"});
}

TEST(TrieStatsTest, CountsZeroWeightDictionaryEntries) {
    const auto keys = std::vector<Unicode>{decode("词"), decode("词条")};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{}}, {0.0f, PosTag{}}};
    auto trie = Trie{};
    trie.build(keys, values);
    EXPECT_EQ(trie.collect_stats().value_count, 2u);
}

TEST(TrieTest, PreservesPrefixValuesWhenTransitionTableGrows) {
    const auto keys = std::vector<Unicode>{{U'a'}, Unicode(512, U'a'), {U'a', U'b'}};
    const auto values = std::vector<DictUnit>{{-1.0f, PosTag{"n"}}, {-2.0f, PosTag{"n"}}, {-3.0f, PosTag{"v"}}};
    auto trie = Trie{};
    trie.build(keys, values);
    for (auto i = size_t{0}; i < keys.size(); ++i) {
        const auto found = trie.find(std::span<const Rune>{keys[i]});
        EXPECT_FLOAT_EQ(found.weight, values[i].weight);
        EXPECT_EQ(found.tag, values[i].tag);
    }
    EXPECT_FALSE(trie.find(decode("aa")).has_value());
    EXPECT_FALSE(trie.find(decode("abc")).has_value());
    EXPECT_FALSE(trie.collect_stats().to_string().empty());
}

TEST(TrieTest, EmptyTrieProducesDagWithoutEdges) {
    const auto trie = Trie{};
    const auto dag = trie.find_dag(decode("𠮷中"));
    ASSERT_EQ(dag.size(), 2u);
    EXPECT_TRUE(dag.get_edges(0).empty());
    EXPECT_TRUE(dag.get_edges(1).empty());
}

TEST(TrieTest, DagIncludesUnknownRunesAndMatchingPrefixes) {
    const auto keys = std::vector<Unicode>{decode("𠮷"), decode("𠮷中")};
    const auto values = std::vector<DictUnit>{{-1.0f, PosTag{"n"}}, {-2.0f, PosTag{"n"}}};
    auto trie = Trie{};
    trie.build(keys, values);
    const auto dag = trie.find_dag(decode("𠮷中外"));
    ASSERT_EQ(dag.size(), 3u);
    const auto matches = dag.get_edges(0);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].next_pos, 1u);
    EXPECT_FLOAT_EQ(matches[0].weight, -1.0f);
    EXPECT_EQ(matches[1].next_pos, 2u);
    EXPECT_FLOAT_EQ(matches[1].weight, -2.0f);
    const auto unknown = dag.get_edges(2);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0].next_pos, 3u);
    EXPECT_EQ(unknown[0].weight, kMissingWordWeight);
}

TEST(TrieStatsTest, FormatsDefaultStatistics) {
    const auto stats = TrieStats{};
    EXPECT_FALSE(stats.to_string().empty());
}

TEST(TrieStatsTest, EmptyKeysProduceRootOnlyStatistics) {
    const auto keys = std::vector<Unicode>(2);
    const auto values = std::vector<DictUnit>(2);
    auto trie = Trie{};
    trie.build(keys, values);
    const auto stats = trie.collect_stats();
    EXPECT_EQ(stats.node_count, 1u);
    EXPECT_EQ(stats.edge_count, 0u);
    EXPECT_EQ(stats.leaf_count, 1u);
    EXPECT_EQ(stats.avg_depth, 0.0);
    EXPECT_FALSE(stats.to_string().empty());
}

TEST(TrieStatsTest, FindsWordsAcrossSmallAndLargeFanouts) {
    const auto fanouts = std::array<size_t, 3>{1, 4, 64};
    for (const auto fanout : fanouts) {
        auto keys = std::vector<Unicode>{};
        auto values = std::vector<DictUnit>{};
        keys.reserve(fanout);
        values.reserve(fanout);
        for (auto i = std::size_t{0}; i < fanout; ++i) {
            keys.push_back(Unicode{static_cast<Rune>(U'\u4E00' + i)});
            values.push_back({static_cast<float>(i + 1), PosTag{"n"}});
        }
        auto trie = Trie{};
        trie.build(keys, values);
        for (auto i = std::size_t{0}; i < keys.size(); ++i) {
            const auto found = trie.find(std::span<const Rune>{keys[i]});
            EXPECT_FLOAT_EQ(found.weight, values[i].weight);
            EXPECT_EQ(found.tag, values[i].tag);
        }
        EXPECT_FALSE(trie.find(U"\u9FFF").has_value());
    }
}

TEST(TrieTest, FindsHighFanoutChildrenAcrossUnicodeRange) {
    const auto boundary_runes = std::array{U'\0', U'中', U'\U0001F600', U'\U0010FFFF'};
    const auto fanout = boundary_runes.size() + 4;
    auto keys = std::vector<Unicode>{};
    auto values = std::vector<DictUnit>{};
    keys.reserve(2 * fanout);
    values.reserve(2 * fanout);
    for (auto i = size_t{0}; i < fanout; ++i) {
        const auto rune = i < boundary_runes.size() ? boundary_runes[i] : static_cast<Rune>(U'\u4E00' + i);
        keys.push_back({rune});
        keys.push_back({U'词', rune});
        values.push_back({static_cast<float>(i), PosTag{"n"}});
        values.push_back({-static_cast<float>(i), PosTag{"v"}});
    }
    auto trie = Trie{};
    trie.build(keys, values);
    for (auto i = size_t{0}; i < keys.size(); ++i) {
        const auto found = trie.find(std::span<const Rune>{keys[i]});
        ASSERT_TRUE(found.has_value());
        EXPECT_FLOAT_EQ(found.weight, values[i].weight);
        EXPECT_EQ(found.tag, values[i].tag);
    }
    EXPECT_FALSE(trie.find(decode("词")).has_value());
    EXPECT_FALSE(trie.find(decode("词外")).has_value());
    EXPECT_FALSE(trie.find(decode("𠮷")).has_value());
    EXPECT_FALSE(trie.find(decode("词𠮷")).has_value());
}

TEST(TrieTest, HighFanoutDagPreservesMatchingPrefixesAndUnknownRunes) {
    auto keys = std::vector<Unicode>{};
    auto values = std::vector<DictUnit>{};
    const auto fanout = size_t{32};
    keys.reserve(fanout);
    values.reserve(fanout);
    for (auto i = size_t{0}; i < fanout; ++i) {
        keys.push_back({U'词', static_cast<Rune>(U'\U00020000' + i)});
        values.push_back({static_cast<float>(i), PosTag{}});
    }
    auto trie = Trie{};
    trie.build(keys, values);
    for (auto i = size_t{0}; i <= fanout; ++i) {
        const auto sentence = Unicode{U'词', static_cast<Rune>(U'\U00020000' + i), U'外'};
        const auto dag = trie.find_dag(std::span<const Rune>{sentence});
        ASSERT_EQ(dag.size(), 3u);
        const auto prefixes = dag.get_edges(0);
        ASSERT_EQ(prefixes.size(), i < fanout ? 2u : 1u);
        EXPECT_EQ(prefixes[0].next_pos, 1u);
        EXPECT_EQ(prefixes[0].weight, kMissingWordWeight);
        if (i < fanout) {
            EXPECT_EQ(prefixes[1].next_pos, 2u);
            EXPECT_FLOAT_EQ(prefixes[1].weight, values[i].weight);
        }
        for (auto pos = size_t{1}; pos < sentence.size(); ++pos) {
            const auto edges = dag.get_edges(pos);
            ASSERT_EQ(edges.size(), 1u);
            EXPECT_EQ(edges[0].next_pos, pos + 1);
            EXPECT_EQ(edges[0].weight, kMissingWordWeight);
        }
    }
}

TEST(TrieTest, DistinguishesRootTableBoundaryAndSupplementaryRunes) {
    const auto keys = std::vector<Unicode>{{U'\0'},         {U'\uFFFF'},        {U'\U00010000'},
                                           {U'\U0010FFFF'}, {U'\uFFFF', U'中'}, {U'\U00010000', U'中'}};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{}},  {-1.0f, PosTag{}}, {-2.0f, PosTag{}},
                                              {-3.0f, PosTag{}}, {-4.0f, PosTag{}}, {-5.0f, PosTag{}}};
    auto trie = Trie{};
    trie.build(keys, values);
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto found = trie.find(std::span<const Rune>{keys[i]});
        ASSERT_TRUE(found.has_value());
        EXPECT_FLOAT_EQ(found.weight, values[i].weight);
    }
    EXPECT_FALSE(trie.find(U"\U00010001").has_value());
    EXPECT_FALSE(trie.find(U"\U0010FFFF中").has_value());
}

TEST(TrieTest, TerminalWordsDoNotReuseRootTransitions) {
    const auto keys = std::vector<Unicode>{decode("𠮷"), decode("😀"), decode("中")};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{}}, {-1.0f, PosTag{}}, {-2.0f, PosTag{}}};
    auto trie = Trie{};
    trie.build(keys, values);
    EXPECT_FALSE(trie.find(decode("𠮷😀")).has_value());
    EXPECT_FALSE(trie.find(decode("中𠮷")).has_value());
    const auto dag = trie.find_dag(decode("𠮷😀中"));
    ASSERT_EQ(dag.size(), 3u);
    for (size_t i = 0; i < dag.size(); ++i) {
        const auto edges = dag.get_edges(i);
        ASSERT_EQ(edges.size(), 1u);
        EXPECT_EQ(edges.front().next_pos, i + 1);
        EXPECT_FLOAT_EQ(edges.front().weight, values[i].weight);
    }
}

TEST(TrieTest, RebuildReplacesDirectAndHashedWords) {
    const auto old_keys = std::vector<Unicode>{decode("中"), decode("𠮷中")};
    const auto old_values = std::vector<DictUnit>{{0.0f, PosTag{}}, {-1.0f, PosTag{}}};
    const auto new_keys = std::vector<Unicode>{decode("新词")};
    const auto new_values = std::vector<DictUnit>{{-2.0f, PosTag{"n"}}};
    auto trie = Trie{};
    trie.build(old_keys, old_values);
    trie.build(new_keys, new_values);
    EXPECT_FALSE(trie.find(decode("中")).has_value());
    EXPECT_FALSE(trie.find(decode("𠮷中")).has_value());
    EXPECT_FLOAT_EQ(trie.find(decode("新词")).weight, -2.0f);
    EXPECT_EQ(trie.collect_stats().value_count, 1u);
    trie.build(std::span<const Unicode>{}, std::span<const DictUnit>{});
    EXPECT_TRUE(trie.empty());
    EXPECT_FALSE(trie.find(decode("新词")).has_value());
    EXPECT_EQ(trie.collect_stats().total_estimated_bytes, 0u);
}

TEST(TrieTest, MovedFromTrieCanBeQueriedAndRebuilt) {
    const auto keys = std::vector<Unicode>{decode("𠮷中")};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{"n"}}};
    auto source = Trie{};
    source.build(keys, values);
    const auto moved = Trie{std::move(source)};
    EXPECT_FLOAT_EQ(moved.find(decode("𠮷中")).weight, 0.0f);
    EXPECT_TRUE(source.empty()); // NOLINT(bugprone-use-after-move): checks the moved-from state.
    EXPECT_EQ(source.node_count(), 0u);
    EXPECT_FALSE(source.find(decode("𠮷中")).has_value());
    EXPECT_TRUE(source.find_dag(decode("𠮷中")).edges.empty());
    EXPECT_EQ(source.collect_stats().edge_count, 0u);
    source.build(keys, values);
    EXPECT_FLOAT_EQ(source.find(decode("𠮷中")).weight, 0.0f);
}

TEST(TrieTest, MoveAssignmentReplacesWordsAcrossDifferentChildCounts) {
    const auto keys = std::vector<Unicode>{decode("甲一"), decode("甲二"),   decode("甲三"), decode("甲四"),
                                           decode("甲五"), decode("甲一乙"), decode("𠮷中")};
    const auto values = std::vector<DictUnit>(keys.size(), DictUnit{.weight = -1.0f});
    auto source = Trie{};
    source.build(keys, values);
    auto destination = Trie{};
    destination.build(std::array{decode("旧词")}, std::array{DictUnit{.weight = -2.0f}});
    destination = std::move(source);

    EXPECT_FALSE(destination.find(decode("旧词")).has_value());
    for (const auto &key : keys) {
        EXPECT_FLOAT_EQ(destination.find(key).weight, -1.0f);
    }
    EXPECT_TRUE(source.empty()); // NOLINT(bugprone-use-after-move): checks the moved-from state.
    EXPECT_FALSE(source.find(decode("甲一")).has_value());
    source.build(keys, values);
    EXPECT_FLOAT_EQ(source.find(decode("甲一乙")).weight, -1.0f);
}

TEST(TrieTest, DagMatchesIndependentDictionaryAcrossSharedUnicodePrefixes) {
    const auto alphabet = std::array{U'\0', U'a', U'b', U'中', U'词', U'𠮷', U'😀', U'\uFFFF', U'\U00010000'};
    auto rng = std::mt19937{42};
    auto keys = std::vector<Unicode>{};
    auto values = std::vector<DictUnit>{};
    auto expected = std::map<std::u32string, DictUnit>{};
    keys.reserve(600);
    values.reserve(600);
    for (size_t i = 0; i < 600; ++i) {
        auto key = Unicode{};
        const auto length = size_t{1} + rng() % 6;
        key.reserve(length);
        for (size_t j = 0; j < length; ++j) {
            key.push_back(alphabet[rng() % alphabet.size()]);
        }
        const auto value = DictUnit{-static_cast<float>(i % 13), PosTag{"n"}};
        expected[std::u32string{key.begin(), key.end()}] = value;
        keys.push_back(std::move(key));
        values.push_back(value);
    }
    auto trie = Trie{};
    trie.build(keys, values);
    for (const auto &[key, value] : expected) {
        const auto found = trie.find(std::span<const Rune>{key});
        EXPECT_FLOAT_EQ(found.weight, value.weight);
        EXPECT_EQ(found.tag, value.tag);
        const auto dag = trie.find_dag(std::span<const Rune>{key});
        ASSERT_EQ(dag.size(), key.size());
        for (size_t i = 0; i < key.size(); ++i) {
            auto expected_edges = std::vector<DagEdge>{};
            auto prefix = std::u32string{};
            for (size_t j = i; j < key.size(); ++j) {
                prefix.push_back(key[j]);
                const auto match = expected.find(prefix);
                if (j == i || match != expected.end()) {
                    expected_edges.push_back({static_cast<RuneIndex>(j + 1),
                                              match != expected.end() ? match->second.weight : kMissingWordWeight});
                }
            }
            const auto actual = dag.get_edges(i);
            ASSERT_EQ(actual.size(), expected_edges.size());
            for (size_t j = 0; j < actual.size(); ++j) {
                EXPECT_EQ(actual[j].next_pos, expected_edges[j].next_pos);
                EXPECT_FLOAT_EQ(actual[j].weight, expected_edges[j].weight);
            }
        }
    }
}

TEST(TrieStatsTest, AccountsForDirectHashedAndCompactTransitions) {
    const auto keys = std::vector<Unicode>{decode("a"), decode("ab"), decode("𠮷"), decode("𠮷中")};
    const auto values = std::vector<DictUnit>(keys.size(), DictUnit{0.0f, PosTag{}});
    auto trie = Trie{};
    trie.build(keys, values);
    const auto stats = trie.collect_stats();
    EXPECT_EQ(stats.node_count, 3u);
    EXPECT_EQ(stats.edge_count, 4u);
    EXPECT_EQ(stats.direct_root_edge_count, 1u);
    EXPECT_EQ(stats.hashed_edge_count, 1u);
    EXPECT_EQ(stats.compact_edge_count, 2u);
    EXPECT_GT(stats.root_table_bytes, 0u);
    EXPECT_LT(stats.root_table_bytes, 4096u);
    EXPECT_GT(stats.transition_table_bytes, 0u);
    EXPECT_GT(stats.compact_table_bytes, 0u);
    EXPECT_EQ(stats.total_estimated_bytes,
              stats.root_table_bytes + stats.transition_table_bytes + stats.compact_table_bytes);
    EXPECT_FALSE(stats.to_string().empty());
}

TEST(TrieStatsTest, SupplementaryRootsNeedNoBmpTable) {
    const auto keys = std::vector<Unicode>{decode("𠮷")};
    const auto values = std::vector<DictUnit>{{0.0f, PosTag{}}};
    auto trie = Trie{};
    trie.build(keys, values);
    const auto stats = trie.collect_stats();
    EXPECT_EQ(stats.root_table_bytes, 0u);
    EXPECT_EQ(stats.direct_root_edge_count, 0u);
    EXPECT_EQ(stats.hashed_edge_count, 1u);
    EXPECT_FLOAT_EQ(trie.find(decode("𠮷")).weight, 0.0f);
}

/// Helper: load jieba.dict.utf8 and build a neo Trie.
/// Format per line: "word freq tag"  (space-delimited, UTF-8).
namespace {
static auto build_jieba_trie() -> std::pair<Trie, size_t> {
    auto dict_path = std::string(DICT_DIR) + "/jieba.dict.utf8";
    auto file = read_file(dict_path);
    auto content = file.content();

    // Collect entries: word, freq, tag
    struct RawEntry {
        std::string_view word;
        double freq;
        std::string_view tag;
    };
    auto entries = std::vector<RawEntry>{};
    auto freq_sum = 0.0;

    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        if (line.empty()) {
            continue;
        }
        auto fields = std::array<std::string_view, 3>{};
        auto fi = size_t{0};
        auto split = get_split_view(line, ' ');
        for (auto &&field : split) {
            if (fi < 3) {
                fields[fi] = field;
            }
            ++fi;
        }
        if (fi < 2) {
            continue;
        }

        auto freq = 0.0;
        auto &&[ptr, ec] = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), freq);
        if (ec != std::errc{}) {
            continue;
        }

        freq_sum += freq;
        entries.push_back({fields[0], freq, fi >= 3 ? fields[2] : std::string_view{}});
    }

    auto keys = std::vector<Unicode>{};
    auto values = std::vector<DictUnit>{};
    keys.reserve(entries.size());
    values.reserve(entries.size());

    for (auto &e : entries) {
        keys.push_back(decode(e.word));
        auto w = e.freq > 0 ? static_cast<float>(std::log(e.freq / freq_sum)) : -20.0f;
        values.push_back({w, PosTag(e.tag)});
    }

    auto trie = Trie{};
    trie.build(keys, values);
    return std::pair{std::move(trie), keys.size()};
}
} // namespace
// ─── Basic TrieStats on a small hand-built trie ─────────────────────────────

TEST(TrieStatsTest, SmallTrie) {
    // Build a tiny trie with 3 keys: "ab", "ac", "abd"
    auto keys = std::vector<Unicode>{
        {U'a', U'b'},
        {U'a', U'c'},
        {U'a', U'b', U'd'},
    };
    auto values = std::vector<DictUnit>{
        {1.0f, PosTag("n")},
        {2.0f, PosTag("v")},
        {3.0f, PosTag("a")},
    };

    auto trie = Trie{};
    trie.build(keys, values);

    auto stats = trie.collect_stats();

    // Trie shape (lazy insert — no child nodes for terminal edges):
    //   root(0) --a--> (1) --b[val]--> (2) --d[val]--> [-1]
    //                       \--c[val]--> [-1]
    // Nodes: 3, Edges: 4, Lazy edges: 2, Leaves: 0, Values: 3
    EXPECT_EQ(stats.node_count, 3u);
    EXPECT_EQ(stats.edge_count, 4u);
    EXPECT_EQ(stats.value_count, 3u);
    EXPECT_EQ(stats.lazy_edge_count, 2u);
    EXPECT_EQ(stats.leaf_count, 0u);

    // Depths: root=0, (1)=1, (2)=2 → max=2
    EXPECT_EQ(stats.max_depth, 2u);

    // Max fanout: node(1) has 2 children → max_fanout = 2
    EXPECT_EQ(stats.max_fanout, 2u);

    // to_string should not be empty
    auto report = stats.to_string();
    EXPECT_FALSE(report.empty());
}

// ─── Empty Trie ──────────────────────────────────────────────────────────────

TEST(TrieStatsTest, EmptyTrie) {
    auto trie = Trie{};
    auto stats = trie.collect_stats();

    EXPECT_EQ(stats.node_count, 0u);
    EXPECT_EQ(stats.edge_count, 0u);
    EXPECT_EQ(stats.value_count, 0u);
    EXPECT_EQ(stats.leaf_count, 0u);
    EXPECT_EQ(stats.max_depth, 0u);
    EXPECT_EQ(stats.max_fanout, 0u);
    EXPECT_EQ(stats.total_estimated_bytes, 0u);
}

// ─── Single-key Trie ─────────────────────────────────────────────────────────

TEST(TrieStatsTest, SingleKey) {
    auto keys = std::vector<Unicode>{{U'x'}};
    auto values = std::vector<DictUnit>{{5.0f, PosTag("n")}};

    auto trie = Trie{};
    trie.build(keys, values);

    auto stats = trie.collect_stats();

    // root(0) --x[val]--> [-1]   nodes=1, edges=1, lazy=1, values=1, leaves=0
    EXPECT_EQ(stats.node_count, 1u);
    EXPECT_EQ(stats.edge_count, 1u);
    EXPECT_EQ(stats.value_count, 1u);
    EXPECT_EQ(stats.lazy_edge_count, 1u);
    EXPECT_EQ(stats.leaf_count, 0u);
    EXPECT_EQ(stats.max_depth, 0u);
    EXPECT_EQ(stats.max_fanout, 1u);
}

// ─── jieba.dict.utf8 full stats ──────────────────────────────────────────────

TEST(TrieStatsTest, JiebaDict) {
    auto &&[trie, dict_size] = build_jieba_trie();

    ASSERT_FALSE(trie.empty());
    ASSERT_GT(dict_size, 300000u); // jieba.dict has ~349k entries

    auto stats = trie.collect_stats();

    // Print the full report for visibility
    std::println("\n{}", stats.to_string());

    // ── Sanity checks on jieba.dict structure ────────────────────────────

    // Node count is smaller than before due to lazy insert (terminal edges
    // don't allocate child nodes). Still more nodes than keys.
    EXPECT_GT(stats.node_count, 100000u); // reduced from old 400k+ due to lazy insert

    // With lazy insert: edge_count = node_count - 1 + lazy_edge_count
    EXPECT_EQ(stats.edge_count, stats.node_count - 1 + stats.lazy_edge_count);

    // Lazy edge count should be significant (many terminal edges).
    EXPECT_GT(stats.lazy_edge_count, 100000u);

    // Value count should match the number of inserted dictionary entries.
    EXPECT_EQ(stats.value_count, dict_size);

    // With lazy insert, leaf_count is 0 (every materialized node has edges).
    EXPECT_EQ(stats.leaf_count, 0u);

    // Chinese words are typically 1-8 characters; max depth shouldn't be extreme.
    EXPECT_GE(stats.max_depth, 2u);
    EXPECT_LE(stats.max_depth, 50u); // generous upper bound

    // Root must have a large fanout (thousands of distinct first-characters in CJK).
    EXPECT_GT(stats.max_fanout, 500u);

    // Average fanout of non-leaf nodes: should be modest (most internal nodes have few children).
    EXPECT_GT(stats.avg_fanout, 1.0);
    EXPECT_LT(stats.avg_fanout, 20.0);

    // Memory: should be in the tens of MB range for ~350k entries.
    EXPECT_GT(stats.total_estimated_bytes, 1'000'000u);   // > 1 MB
    EXPECT_LT(stats.total_estimated_bytes, 500'000'000u); // < 500 MB

    // Depth histogram should have entries.
    EXPECT_FALSE(stats.depth_histogram.empty());
    // Root (depth 0) has exactly 1 node.
    EXPECT_EQ(stats.depth_histogram[0], 1u);

    // Fanout histogram should have entries.
    EXPECT_FALSE(stats.fanout_histogram.empty());

    // Average load factor should be between 0 and 1 (healthy hash maps).
    EXPECT_GT(stats.transition_load_factor, 0.0);
    EXPECT_LE(stats.transition_load_factor, 1.0);
}
