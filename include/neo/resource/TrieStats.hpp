#pragma once

#include "neo/util/Logging.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace neo_cppjieba::detail {
/// Structural statistics of a Trie, capturing the key factors that affect
/// query performance: branching factor distribution, depth profile, memory
/// layout, and load-factor characteristics of the internal hash maps.
struct TrieStats {
    // ── Basic counts ─────────────────────────────────────────────────────
    size_t node_count = 0;  ///< Total number of nodes (including root).
    size_t edge_count = 0;  ///< Total number of edges (sum of all children sizes).
    size_t value_count = 0; ///< Number of transitions that carry a DictUnit value.
    size_t leaf_count = 0;  ///< Nodes with zero children.

    // ── Depth (distance from root) ───────────────────────────────────────
    size_t max_depth = 0;
    double avg_depth = 0.0;              ///< Average depth across all nodes.
    double avg_leaf_depth = 0.0;         ///< Average depth of leaf nodes.
    std::vector<size_t> depth_histogram; ///< depth_histogram[d] = #nodes at depth d.

    // ── Branching factor (children count per node) ───────────────────────
    size_t max_fanout = 0;                ///< Largest children count of any single node.
    double avg_fanout = 0.0;              ///< Average children count (over non-leaf nodes).
    std::vector<size_t> fanout_histogram; ///< fanout_histogram[f] = #nodes with f children.

    // ── Memory estimation (bytes) ────────────────────────────────────────
    size_t root_table_bytes = 0;       ///< Storage for direct BMP root transitions.
    size_t transition_table_bytes = 0; ///< Estimated flat hash table slots and control bytes.
    size_t total_estimated_bytes = 0;  ///< root_table_bytes + transition_table_bytes.
    // The total now also includes compact entries and their per-node container offsets.
    size_t compact_table_bytes = 0; ///< Storage for compact child arrays and offsets.

    // ── Hash map health ──────────────────────────────────────────────────
    double transition_load_factor = 0.0; ///< Load factor of the shared transition table.
    size_t transition_capacity = 0;      ///< Number of slots in the shared transition table.

    // ── Lazy insert savings ───────────────────────────────────────────────
    size_t lazy_edge_count = 0; ///< Terminal edges without a materialized child node.

    // ── Hybrid storage ───────────────────────────────────────────────────
    size_t direct_root_edge_count = 0; ///< Root transitions stored in the BMP table.
    size_t hashed_edge_count = 0;      ///< Other transitions stored in the flat hash table.
    size_t compact_edge_count = 0;     ///< Transitions stored in compact child arrays.

    /// Pretty-print the stats to a string.
    [[nodiscard]]
    auto to_string() const -> std::string {
        assert_check([this] { return leaf_count <= node_count; }, "TrieStats: leaf count exceeds the node count");
        assert_check([this] { return value_count <= edge_count && lazy_edge_count <= edge_count; },
                     "TrieStats: inconsistent edge counts");
        assert_check(
            [this] {
                return direct_root_edge_count <= edge_count && compact_edge_count <= edge_count - direct_root_edge_count
                       && hashed_edge_count == edge_count - direct_root_edge_count - compact_edge_count;
            },
            "TrieStats: storage counts must cover all transitions");
        assert_check(
            [this] {
                return root_table_bytes <= total_estimated_bytes
                       && compact_table_bytes <= total_estimated_bytes - root_table_bytes
                       && transition_table_bytes == total_estimated_bytes - root_table_bytes - compact_table_bytes;
            },
            "TrieStats: inconsistent memory estimates");
        assert_check([this] { return std::isfinite(avg_depth) && std::isfinite(avg_leaf_depth); },
                     "TrieStats: non-finite depth statistics");
        assert_check([this] { return std::isfinite(avg_fanout) && std::isfinite(transition_load_factor); },
                     "TrieStats: non-finite branching or load statistics");
        std::string s;
        s.reserve(2048);

        auto line = [&](std::string_view label, auto value) {
            if constexpr (std::is_floating_point_v<decltype(value)>) {
                s += std::format("  {:<30}  {:>12.2f}\n", label, static_cast<double>(value));
            } else {
                s += std::format("  {:<30}  {:>12}\n", label, static_cast<size_t>(value));
            }
        };

        s += "┌─────────────────────────────────────────────────┐\n";
        s += "│              Trie Structure Stats               │\n";
        s += "├─────────────────────────────────────────────────┤\n";

        line("Node count", node_count);
        line("Edge count", edge_count);
        line("Value count", value_count);
        line("Leaf count", leaf_count);

        s += "├─────────────────────────────────────────────────┤\n";

        line("Max depth", max_depth);
        line("Avg depth (all nodes)", avg_depth);
        line("Avg depth (leaf nodes)", avg_leaf_depth);

        s += "├─────────────────────────────────────────────────┤\n";

        line("Max fanout", max_fanout);
        line("Avg fanout (non-leaf)", avg_fanout);

        s += "├─────────────────────────────────────────────────┤\n";

        line("Direct root bytes", root_table_bytes);
        line("Transition table bytes", transition_table_bytes);
        line("Compact children bytes", compact_table_bytes);
        line("Total estimated bytes", total_estimated_bytes);
        s += std::format("  {:<30}  {:>9.2f} MB\n", "Total estimated",
                         static_cast<double>(total_estimated_bytes) / (1024.0 * 1024.0));

        s += "├─────────────────────────────────────────────────┤\n";

        line("Lazy edge count", lazy_edge_count);

        s += "├─────────────────────────────────────────────────┤\n";

        line("Hash table capacity", transition_capacity);
        line("Hash table load factor", transition_load_factor);
        line("Direct root edges", direct_root_edge_count);
        line("Hashed edges", hashed_edge_count);
        line("Compact edges", compact_edge_count);

        s += "├─────────────────────────────────────────────────┤\n";

        // Depth histogram (compact: show non-zero entries)
        s += "  Depth histogram:\n";
        for (size_t d = 0; d < depth_histogram.size(); ++d) {
            if (depth_histogram[d] > 0) {
                s += std::format("    depth {:3} : {}\n", d, depth_histogram[d]);
            }
        }

        s += "├─────────────────────────────────────────────────┤\n";

        // Fanout histogram (compact: show selected entries)
        s += "  Fanout histogram (top entries):\n";
        // Gather non-zero entries
        std::vector<std::pair<size_t, size_t>> fanout_entries;
        for (size_t f = 0; f < fanout_histogram.size(); ++f) {
            if (fanout_histogram[f] > 0) {
                fanout_entries.emplace_back(f, fanout_histogram[f]);
            }
        }
        // Show first 15 and last 5 if too many
        auto format_fanout = [&](size_t f, size_t cnt) {
            s += std::format("    fanout {:5} : {}\n", f, cnt);
        };
        if (fanout_entries.size() <= 20) {
            for (auto &[f, cnt] : fanout_entries) {
                format_fanout(f, cnt);
            }
        } else {
            for (size_t i = 0; i < 15; ++i) {
                format_fanout(fanout_entries[i].first, fanout_entries[i].second);
            }
            s += "    ...\n";
            for (size_t i = fanout_entries.size() - 5; i < fanout_entries.size(); ++i) {
                format_fanout(fanout_entries[i].first, fanout_entries[i].second);
            }
        }

        s += "└─────────────────────────────────────────────────┘\n";
        return s;
    }
};
} // namespace neo_cppjieba::detail
