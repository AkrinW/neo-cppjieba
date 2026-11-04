#pragma once

#include "neo/Config.hpp"
#include "neo/concept/Match.hpp"
#include "neo/resource/Dag.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/TrieStats.hpp"
#include "neo/third_party/gtl.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace neo_cppjieba::detail {

/// A transition retains the full parent id and rune independently of the configured node width.
template <CapacityInteger NodeId>
struct TrieTransitionKey {
    NodeId parent;
    Rune rune;

    auto operator==(const TrieTransitionKey &) const noexcept -> bool = default;
};

/// Narrow ids retain the packed-key hash; wider ids contribute all 64-bit chunks to the hash.
struct TrieTransitionHash {
    template <CapacityInteger NodeId>
    auto operator()(const TrieTransitionKey<NodeId> &key) const noexcept -> std::size_t {
        const auto rune = static_cast<std::uint32_t>(key.rune);
        if constexpr (sizeof(NodeId) <= sizeof(std::uint32_t)) {
            const auto packed = (static_cast<std::uint64_t>(key.parent) << 32) | rune;
            return gtl::Hash<std::uint64_t>{}(packed);
        } else if constexpr (sizeof(NodeId) <= sizeof(std::uint64_t)) {
            return gtl::HashState::combine(0, key.parent, rune);
        } else {
            return gtl::HashState::combine(0, static_cast<std::uint64_t>(key.parent),
                                           static_cast<std::uint64_t>(key.parent >> 64), rune);
        }
    }
};

/// One edge of a trie stored breadth-first. Nodes are numbered in level order and each node's edges are sorted
/// by rune, so the k-th edge with a child reaches node k and child ids need no storage.
struct LevelOrderEdge {
    Rune rune{};
    bool has_child{false};
    bool has_value{false};

    auto operator==(const LevelOrderEdge &) const noexcept -> bool = default;
};

/// A trie shape in level order: fanouts[node] consecutive edges per node, starting with the root, node 0.
struct LevelOrderShape {
    std::vector<std::uint32_t> fanouts;
    std::vector<LevelOrderEdge> edges;

    auto operator==(const LevelOrderShape &) const -> bool = default;
};

/// The level-order shape of a key set, with the index of the key that ends at each valued edge, in edge order.
struct LevelOrderKeys {
    LevelOrderShape shape;
    std::vector<std::size_t> value_keys;
};

// Build the level-order shape of nonempty, strictly ascending keys. Each node covers the sorted range of keys
// longer than its depth that extend its prefix; a key equal to an edge's prefix sorts first and ends there.
[[nodiscard]]
inline auto level_order_keys(std::span<const Unicode> keys) -> LevelOrderKeys {
    assert_check(
        [&] {
            return std::ranges::none_of(keys, [](const Unicode &key) { return key.empty(); })
                   && std::ranges::adjacent_find(keys, std::ranges::greater_equal{}) == keys.end();
        },
        "Trie: level-order keys must be nonempty and strictly ascending");
    /// The keys below one node, all longer than its depth.
    struct NodeRange {
        std::size_t begin;
        std::size_t end;
        std::size_t depth;
    };
    auto result = LevelOrderKeys{};
    auto nodes = std::vector<NodeRange>{{0, keys.size(), 0}};
    for (auto node = std::size_t{0}; node < nodes.size(); ++node) {
        const auto [begin, end, depth] = nodes[node];
        auto fanout = std::uint32_t{0};
        for (auto first = begin; first < end; ++fanout) {
            const auto rune = keys[first][depth];
            auto last = first + 1;
            while (last < end && keys[last][depth] == rune) {
                ++last;
            }
            const auto has_value = keys[first].size() == depth + 1;
            const auto child_begin = has_value ? first + 1 : first;
            result.shape.edges.push_back(LevelOrderEdge{rune, child_begin < last, has_value});
            if (has_value) {
                result.value_keys.push_back(first);
            }
            if (child_begin < last) {
                nodes.push_back(NodeRange{child_begin, last, depth + 1});
            }
            first = last;
        }
        result.shape.fanouts.push_back(fanout);
    }
    return result;
}

// Check one node's edges: Unicode scalar runes in strictly ascending order, each leading to a child or a value.
[[nodiscard]]
inline auto level_order_node_error(std::span<const LevelOrderEdge> edges) -> std::string_view {
    if (edges.empty()) {
        return "a trie node has no edges";
    }
    if (std::ranges::adjacent_find(edges, std::ranges::greater_equal{}, &LevelOrderEdge::rune) != edges.end()) {
        return "edge runes are not strictly ascending within a node";
    }
    for (const auto &edge : edges) {
        if (!is_unicode_scalar(edge.rune)) {
            return "an edge rune is not a Unicode scalar value";
        }
        if (!edge.has_child && !edge.has_value) {
            return "an edge has neither a child nor a value";
        }
    }
    return {};
}

// Describe the first violation of the level-order invariants Trie::build_level_order relies on, or return an
// empty view. A node is reachable only if an edge of an earlier node leads to it, which also rules out cycles.
[[nodiscard]]
inline auto level_order_shape_error(const LevelOrderShape &shape) -> std::string_view {
    const auto &[fanouts, edges] = shape;
    if (fanouts.empty()) {
        return "the trie has no root node";
    }
    if (!detail::size_below_capacity<TrieNodeId>(fanouts.size() - 1)) {
        return "the node count exceeds the configured trie node range";
    }
    auto begin = std::size_t{0};
    auto next_child = std::size_t{1};
    for (auto node = std::size_t{0}; node < fanouts.size(); ++node) {
        if (node > 0 && next_child <= node) {
            return "a trie node is not reached from an earlier node";
        }
        const auto fanout = static_cast<std::size_t>(fanouts[node]);
        if (fanout > edges.size() - begin) {
            return "node fanouts exceed the edge count";
        }
        const auto node_edges = std::span{edges}.subspan(begin, fanout);
        if (const auto error = level_order_node_error(node_edges); !error.empty()) {
            return error;
        }
        next_child += static_cast<std::size_t>(std::ranges::count_if(node_edges, &LevelOrderEdge::has_child));
        begin += fanout;
    }
    if (begin != edges.size()) {
        return "edges remain after the last node";
    }
    if (next_child != fanouts.size()) {
        return "child edges do not match the node count";
    }
    return {};
}

class Trie;

// Each lookup is written once for the trie and its const view.
template <typename Self>
concept TrieSelf = std::same_as<std::remove_const_t<Self>, Trie>;

/// A read-only character trie with direct BMP root transitions and one flat hash table.
/// Each transition carries its dictionary value and a filter for the child's outgoing runes.
class Trie {
    static constexpr auto kAbsentNode = std::numeric_limits<TrieNodeId>::max();

    /// The maximum child id marks an absent root slot; zero marks a terminal edge without a child node.
    struct Entry {
        TrieNodeId child{kAbsentNode};
        uint32_t filter{0};
        DictUnit value{};
    };
    static_assert(sizeof(TrieNodeId) > sizeof(std::uint32_t) || sizeof(Entry) == 8 + sizeof(DictUnit));

    /// Small child sets keep complete entries together without per-edge hash keys.
    struct CompactTransition {
        Rune rune{};
        Entry entry;
    };
    static constexpr auto kCompactFanout = size_t{4};
    static constexpr auto kCompactFlag = uint32_t{1} << 31;
    static constexpr auto kFilterMask = ~kCompactFlag;

    using TransitionKey = detail::TrieTransitionKey<TrieNodeId>;
    using Transitions = gtl::flat_hash_map<TransitionKey, Entry, detail::TrieTransitionHash>;
    static constexpr size_t kRootTableLimit = 0x10000;

    std::vector<Entry> root_;
    Transitions transitions_;
    std::vector<size_t> compact_offsets_;
    std::vector<CompactTransition> compact_;
    size_t node_count_{0};

    /// Keep all rune bits separate from the parent id; root transitions use parent id zero.
    [[nodiscard]]
    static constexpr auto transition_key(TrieNodeId parent, Rune rune) noexcept -> TransitionKey {
        assert_check([&] { return parent != kAbsentNode; }, "Trie: absent transition parent");
        return {parent, rune};
    }

    /// Two filter bits per rune, one in each 16-bit half; collisions only add a table lookup.
    /// Queries mask out the top bit, which selects compact or hashed child storage.
    [[nodiscard]]
    static constexpr auto child_bits(Rune rune) noexcept -> uint32_t {
        const uint32_t hash = static_cast<uint32_t>(rune) * uint32_t{0x9E3779B1};
        return (uint32_t{1} << (hash >> 28)) | (uint32_t{1} << (16 + ((hash >> 24) & 15)));
    }

    /// A trie entry with the constness of the trie it was found in.
    template <TrieSelf Self>
    using EntryOf = std::conditional_t<std::is_const_v<Self>, const Entry, Entry>;

    // Each lookup is implemented once for Self = Trie or const Trie. Queries use the const find_* wrappers;
    // runtime edits use the explicitly named find_mutable_* wrappers.
    template <TrieSelf Self>
    [[nodiscard]]
    static auto root_in(Self &self, Rune rune) noexcept -> EntryOf<Self> * {
        assert_check([&] { return self.node_count_ > 0; }, "Trie: root lookup on an empty trie");
        if (rune < self.root_.size()) {
            auto &entry = self.root_[rune];
            return entry.child != kAbsentNode ? &entry : nullptr;
        }
        const auto it = self.transitions_.find(transition_key(0, rune));
        return it != self.transitions_.end() ? &it->second : nullptr;
    }

    // The filter tag selects storage without reserving any bits from the configured node id.
    template <TrieSelf Self>
    [[nodiscard]]
    static auto transition_in(Self &self, const Entry &parent, Rune rune) noexcept -> EntryOf<Self> * {
        assert(parent.child > 0 && parent.child < self.node_count_);
        if ((parent.filter & kCompactFlag) != 0) {
            const auto id = static_cast<size_t>(parent.child);
            const auto begin = self.compact_offsets_[id];
            const auto end = self.compact_offsets_[id + 1];
            const auto children = std::span{self.compact_}.subspan(begin, end - begin);
            const auto it = std::ranges::find(children, rune, &CompactTransition::rune);
            return it != children.end() ? &it->entry : nullptr;
        }
        const auto it = self.transitions_.find(transition_key(parent.child, rune));
        return it != self.transitions_.end() ? &it->second : nullptr;
    }

    template <TrieSelf Self>
    [[nodiscard]]
    static auto child_in(Self &self, const Entry &parent, Rune rune) noexcept -> EntryOf<Self> * {
        assert_check([&] { return parent.child != kAbsentNode && parent.child < self.node_count_; },
                     "Trie: invalid transition parent");
        const auto bits = child_bits(rune) & kFilterMask;
        if ((parent.filter & bits) != bits) {
            assert_check([&] { return parent.child == 0 || transition_in(self, parent, rune) == nullptr; },
                         "Trie: child filter rejected an existing rune");
            return nullptr;
        }
        assert_check([&] { return parent.child > 0; }, "Trie: terminal edge has a nonempty child filter");
        return transition_in(self, parent, rune);
    }

    template <TrieSelf Self>
    [[nodiscard]]
    static auto entry_in(Self &self, std::span<const Rune> key) noexcept -> EntryOf<Self> * {
        if (self.empty() || key.empty()) {
            return nullptr;
        }
        auto *entry = root_in(self, key.front());
        for (size_t i = 1; entry && i < key.size(); ++i) {
            entry = child_in(self, *entry, key[i]);
        }
        return entry;
    }

    [[nodiscard]]
    auto find_root(Rune rune) const noexcept -> const Entry * {
        return root_in(*this, rune);
    }

    [[nodiscard]]
    auto find_transition(const Entry &parent, Rune rune) const noexcept -> const Entry * {
        return transition_in(*this, parent, rune);
    }

    [[nodiscard]]
    auto find_child(const Entry &parent, Rune rune) const noexcept -> const Entry * {
        return child_in(*this, parent, rune);
    }

    [[nodiscard]]
    auto find_entry(std::span<const Rune> key) const noexcept -> const Entry * {
        return entry_in(*this, key);
    }

    // Runtime edits need exclusive access to the trie while they hold a mutable entry.
    [[nodiscard]]
    auto find_mutable_transition(const Entry &parent, Rune rune) noexcept -> Entry * {
        return transition_in(*this, parent, rune);
    }

    [[nodiscard]]
    auto find_mutable_entry(std::span<const Rune> key) noexcept -> Entry * {
        return entry_in(*this, key);
    }

    /// The returned reference is consumed before the next insertion, which may rehash the table.
    auto ensure_transition(TrieNodeId parent, Rune rune) -> Entry & {
        if (parent == 0 && rune < kRootTableLimit) {
            assert_check([&] { return rune < root_.size(); }, "Trie: root table does not cover a dictionary rune");
            auto &entry = root_[rune];
            if (entry.child == kAbsentNode) {
                entry.child = 0;
            }
            return entry;
        }
        return transitions_.try_emplace(transition_key(parent, rune), Entry{0, 0, {}}).first->second;
    }

    // Count the child nodes an insertion would allocate; an existing path allocates none.
    [[nodiscard]]
    auto new_node_count(std::span<const Rune> key) const noexcept -> size_t {
        assert_check([&] { return !key.empty(); }, "Trie: invalid insertion key");
        if (empty()) {
            // The root node is recreated on insertion; every edge except the last needs a child node.
            return key.size() - 1;
        }
        const auto *entry = find_root(key.front());
        for (size_t i = 1; i < key.size(); ++i) {
            if (entry == nullptr || entry->child == 0) {
                // Edges i - 1 through key.size() - 2 each need a new child node.
                return key.size() - i;
            }
            entry = find_transition(*entry, key[i]);
        }
        return 0;
    }

    // New nodes use hashed child storage, so each one only extends the compact offsets with an empty range.
    auto allocate_node() -> TrieNodeId {
        assert_check([&] { return detail::size_below_capacity<TrieNodeId>(node_count_); },
                     "Trie: node count exceeds the configured index range");
        if (compact_offsets_.empty()) {
            compact_offsets_.resize(node_count_ + 1);
        }
        compact_offsets_.push_back(compact_offsets_.back());
        return static_cast<TrieNodeId>(node_count_++);
    }

    // Compact child sets cannot grow in place; move one into the hash table and leave its slots absent.
    auto promote_children(TrieNodeId node) -> void {
        const auto id = static_cast<size_t>(node);
        assert(id + 1 < compact_offsets_.size());
        for (auto i = compact_offsets_[id]; i < compact_offsets_[id + 1]; ++i) {
            auto &slot = compact_[i];
            transitions_.emplace(transition_key(node, slot.rune), slot.entry);
            slot.entry = Entry{};
        }
    }

    // Runtime insertion may introduce a BMP root beyond the extent sized by build().
    auto ensure_root(Rune rune) -> Entry & {
        if (rune < kRootTableLimit && rune >= root_.size()) {
            root_.resize(static_cast<size_t>(rune) + 1);
        }
        return ensure_transition(0, rune);
    }

    // Parent fields are updated before any table insertion, which may move the parent entry itself.
    auto ensure_child(Entry &parent, Rune rune) -> Entry & {
        assert_check([&] { return parent.child != kAbsentNode && parent.child < node_count_; },
                     "Trie: invalid insertion parent");
        if (parent.child > 0) {
            if (auto *found = find_mutable_transition(parent, rune)) {
                return *found;
            }
        } else {
            parent.child = allocate_node();
        }
        const auto node = parent.child;
        const auto compact = (parent.filter & kCompactFlag) != 0;
        parent.filter = (parent.filter | child_bits(rune)) & kFilterMask;
        if (compact) {
            promote_children(node);
        }
        return ensure_transition(node, rune);
    }

    // Visit every edge as (parent, rune, entry) across direct roots, hashed and compact storage.
    // Slots vacated by runtime promotion stay allocated but hold no transition, so they are skipped.
    template <typename Visit>
        requires std::invocable<Visit &, TrieNodeId, Rune, const Entry &>
    auto for_each_edge(Visit &&visit) const -> void {
        for (size_t rune = 0; rune < root_.size(); ++rune) {
            if (root_[rune].child != kAbsentNode) {
                visit(TrieNodeId{0}, static_cast<Rune>(rune), root_[rune]);
            }
        }
        for (const auto &[key, entry] : transitions_) {
            visit(key.parent, key.rune, entry);
        }
        for (auto parent = size_t{1}; parent < node_count_; ++parent) {
            for (auto i = compact_offsets_[parent]; i < compact_offsets_[parent + 1]; ++i) {
                if (compact_[i].entry.child != kAbsentNode) {
                    visit(static_cast<TrieNodeId>(parent), compact_[i].rune, compact_[i].entry);
                }
            }
        }
    }

    /// Words reconstructed from the trie, in the parallel layout build() takes.
    struct WordList {
        std::vector<Unicode> keys;
        std::vector<DictUnit> values;
    };

    // Every node has exactly one incoming edge, so walking parent edges spells each valued key.
    [[nodiscard]]
    auto live_words() const -> WordList {
        /// The edge that reaches one node.
        struct IncomingEdge {
            TrieNodeId parent{0};
            Rune rune{};
        };
        /// An edge that ends a dictionary word.
        struct ValueEdge {
            TrieNodeId parent;
            Rune rune;
            DictUnit value;
        };
        auto incoming = std::vector<IncomingEdge>(node_count_);
        auto valued = std::vector<ValueEdge>{};
        for_each_edge([&](TrieNodeId parent, Rune rune, const Entry &entry) {
            if (entry.child != 0) {
                incoming[static_cast<size_t>(entry.child)] = IncomingEdge{parent, rune};
            }
            if (entry.value.has_value()) {
                valued.push_back(ValueEdge{parent, rune, entry.value});
            }
        });

        auto words = WordList{};
        words.keys.reserve(valued.size());
        words.values.reserve(valued.size());
        for (const auto &edge : valued) {
            auto key = Unicode{edge.rune};
            for (auto node = edge.parent; node != 0; node = incoming[static_cast<size_t>(node)].parent) {
                key.push_back(incoming[static_cast<size_t>(node)].rune);
            }
            std::ranges::reverse(key);
            words.keys.push_back(std::move(key));
            words.values.push_back(edge.value);
        }
        return words;
    }

    // Copying is reserved for clone(), so whole-dictionary copies stay explicit at call sites.
    Trie(const Trie &) = default;

    auto insert_word(std::span<const Rune> key, DictUnit value, std::vector<uint32_t> &filters) -> void {
        auto parent = TrieNodeId{0};
        for (size_t i = 0; i < key.size(); ++i) {
            assert_check([&] { return parent != kAbsentNode && parent < filters.size(); },
                         "Trie: invalid build node index");
            auto &entry = ensure_transition(parent, key[i]);
            filters[static_cast<size_t>(parent)] |= child_bits(key[i]);
            if (i + 1 == key.size()) {
                // A duplicate key replaces the value: the last entry wins.
                entry.value = value;
                return;
            }
            if (entry.child == 0) {
                // Lazy materialization: allocate a child node now.
                assert_check([&] { return detail::size_below_capacity<TrieNodeId>(filters.size()); },
                             "Trie: node count exceeds the configured index range");
                entry.child = static_cast<TrieNodeId>(filters.size());
                filters.push_back(0);
            }
            parent = entry.child;
        }
    }

    // The compact ranges are finalized before any entry receives its storage tag.
    auto mark_child_storage(Entry &entry) const noexcept -> void {
        entry.filter &= kFilterMask;
        if (entry.child == kAbsentNode || entry.child == 0) {
            return;
        }
        const auto id = static_cast<size_t>(entry.child);
        assert(id + 1 < compact_offsets_.size());
        if (compact_offsets_[id] != compact_offsets_[id + 1]) {
            entry.filter |= kCompactFlag;
        }
    }

    // Freeze small child sets while the trie is private to build(), then shrink the shared table.
    auto compact_children() -> void {
        assert(node_count_ > 0);
        if (node_count_ == 1) {
            return;
        }
        compact_offsets_.resize(node_count_ + 1);
        for (const auto &[key, entry] : transitions_) {
            ++compact_offsets_[static_cast<size_t>(key.parent) + 1];
        }
        // Supplementary root transitions remain hashed; BMP roots already have direct storage.
        compact_offsets_[1] = 0;
        for (auto &count : compact_offsets_) {
            if (count > kCompactFanout) {
                count = 0;
            }
        }
        std::partial_sum(compact_offsets_.begin(), compact_offsets_.end(), compact_offsets_.begin());
        compact_.resize(compact_offsets_.back());
        {
            auto cursors = compact_offsets_;
            for (auto it = transitions_.begin(); it != transitions_.end();) {
                const auto parent = static_cast<size_t>(it->first.parent);
                if (compact_offsets_[parent] == compact_offsets_[parent + 1]) {
                    ++it;
                    continue;
                }
                compact_[cursors[parent]++] = CompactTransition{it->first.rune, it->second};
                transitions_.erase(it++);
            }
        }
        // rehash(0) can produce a nearly full table. Keep at least two slots per remaining transition.
        assert(transitions_.size() <= std::numeric_limits<size_t>::max() / 2);
        auto remaining = Transitions{};
        remaining.rehash(transitions_.size() * 2);
        for (const auto &[key, entry] : transitions_) {
            remaining.emplace(key, entry);
        }
        transitions_ = std::move(remaining);
        for (auto &entry : root_) {
            mark_child_storage(entry);
        }
        for (auto &[key, entry] : transitions_) {
            mark_child_storage(entry);
        }
        for (auto &transition : compact_) {
            mark_child_storage(transition.entry);
        }
    }

    // Size every table before placement: BMP roots get direct slots, child sets of at most kCompactFanout runes
    // get compact ranges, and the hash table keeps two slots per remaining transition, as compact_children does.
    auto reserve_level_order_storage(const LevelOrderShape &shape) -> void {
        const auto roots = std::span{shape.edges}.first(shape.fanouts.front());
        const auto supplementary =
            std::ranges::partition_point(roots, [](const LevelOrderEdge &edge) { return edge.rune < kRootTableLimit; });
        root_.resize(supplementary == roots.begin() ? 0 : static_cast<size_t>(std::prev(supplementary)->rune) + 1);
        auto hashed = static_cast<size_t>(roots.end() - supplementary);
        if (node_count_ > 1) {
            compact_offsets_.resize(node_count_ + 1);
            for (auto node = size_t{1}; node < node_count_; ++node) {
                const auto fanout = static_cast<size_t>(shape.fanouts[node]);
                if (fanout <= kCompactFanout) {
                    compact_offsets_[node + 1] = fanout;
                } else {
                    hashed += fanout;
                }
            }
            std::partial_sum(compact_offsets_.begin(), compact_offsets_.end(), compact_offsets_.begin());
            compact_.reserve(compact_offsets_.back());
        }
        assert(hashed <= std::numeric_limits<size_t>::max() / 2);
        transitions_.rehash(hashed * 2);
    }

    // Each node's filter covers the runes of its own edges, before any storage tag.
    [[nodiscard]]
    static auto level_order_filters(const LevelOrderShape &shape) -> std::vector<uint32_t> {
        auto filters = std::vector<uint32_t>(shape.fanouts.size());
        auto begin = size_t{0};
        for (size_t node = 0; node < shape.fanouts.size(); ++node) {
            for (const auto &edge : std::span{shape.edges}.subspan(begin, shape.fanouts[node])) {
                filters[node] |= child_bits(edge.rune);
            }
            begin += shape.fanouts[node];
        }
        return filters;
    }

    // The filter an edge to child carries, tagged as mark_child_storage would once compact ranges are sized;
    // terminal edges carry none.
    [[nodiscard]]
    auto level_order_filter(std::span<const uint32_t> filters, size_t child) const noexcept -> uint32_t {
        if (child == 0) {
            return 0;
        }
        const auto compact = compact_offsets_[child] != compact_offsets_[child + 1];
        return (filters[child] & kFilterMask) | (compact ? kCompactFlag : 0);
    }

    // Store one edge where build_storage and compact_children would place it.
    auto place_level_order_edge(size_t node, Rune rune, const Entry &entry) -> void {
        if (node == 0 && rune < kRootTableLimit) {
            root_[rune] = entry;
        } else if (node > 0 && compact_offsets_[node] != compact_offsets_[node + 1]) {
            compact_.push_back(CompactTransition{rune, entry});
        } else {
            transitions_.emplace(transition_key(static_cast<TrieNodeId>(node), rune), entry);
        }
    }

    // Edges arrive grouped by node in id order, so compact ranges fill by appending, and counting child edges
    // yields each child id. Filters are tagged on placement, which spares build_storage's final tagging pass.
    auto build_level_order_storage(const LevelOrderShape &shape, std::span<const DictUnit> values) -> void {
        node_count_ = shape.fanouts.size();
        reserve_level_order_storage(shape);
        const auto filters = level_order_filters(shape);
        auto edge = shape.edges.begin();
        auto value = values.begin();
        auto next_child = size_t{1};
        for (size_t node = 0; node < node_count_; ++node) {
            for (const auto end = edge + shape.fanouts[node]; edge != end; ++edge) {
                const auto child = edge->has_child ? next_child++ : size_t{0};
                const auto unit = edge->has_value ? *value++ : DictUnit{};
                place_level_order_edge(node, edge->rune,
                                       Entry{static_cast<TrieNodeId>(child), level_order_filter(filters, child), unit});
            }
        }
    }

    auto build_storage(std::span<const Unicode> keys, std::span<const DictUnit> values) -> void {
        if (keys.empty()) {
            return;
        }
        size_t root_extent = 0;
        size_t word_count = 0;
        for (const auto &key : keys) {
            if (key.empty()) {
                continue;
            }
            ++word_count;
            if (key.front() < kRootTableLimit) {
                root_extent = std::max(root_extent, static_cast<size_t>(key.front()) + 1);
            }
        }
        root_.resize(root_extent);
        transitions_.reserve(word_count);
        auto filters = std::vector<uint32_t>{0};
        assert_check([&] { return word_count <= filters.max_size(); },
                     "Trie: too many dictionary words to reserve build nodes");
        filters.reserve(word_count);
        for (size_t i = 0; i < keys.size(); ++i) {
            insert_word(keys[i], values[i], filters);
        }
        const auto set_filter = [&](Entry &entry) {
            assert_check([&] { return entry.child == kAbsentNode || entry.child < filters.size(); },
                         "Trie: invalid built child index");
            if (entry.child != kAbsentNode && entry.child > 0) {
                entry.filter = filters[static_cast<size_t>(entry.child)];
                assert_check([&] { return entry.filter != 0; }, "Trie: materialized node has no children");
            }
        };
        for (auto &entry : root_) {
            set_filter(entry);
        }
        for (auto &[key, entry] : transitions_) {
            set_filter(entry);
        }
        node_count_ = filters.size();
        compact_children();
    }

public:
    explicit Trie() = default;
    auto operator=(const Trie &) -> Trie & = delete;

    Trie(Trie &&other) noexcept
        : root_{std::move(other.root_)}, transitions_{std::move(other.transitions_)},
          compact_offsets_{std::move(other.compact_offsets_)}, compact_{std::move(other.compact_)},
          node_count_{std::exchange(other.node_count_, 0)} {
    }

    auto operator=(Trie &&other) noexcept -> Trie & {
        if (this != &other) {
            root_ = std::move(other.root_);
            transitions_ = std::move(other.transitions_);
            compact_offsets_ = std::move(other.compact_offsets_);
            compact_ = std::move(other.compact_);
            node_count_ = std::exchange(other.node_count_, 0);
        }
        return *this;
    }

    /// Build from parallel key/value arrays. Empty keys are skipped; duplicate keys use the last value.
    /// Publish only a complete trie, so allocation or size failures leave the previous dictionary intact.
    auto build(std::span<const Unicode> keys, std::span<const DictUnit> values) -> void {
        assert_check([&] { return keys.size() == values.size(); }, "Trie: keys and values must have equal sizes");
        auto next = Trie{};
        next.build_storage(keys, values);
        *this = std::move(next);
    }

    /// Build from a level-order shape and one value per valued edge, in edge order, with the lookups build()
    /// would give for the same words. External shapes must pass level_order_shape_error first.
    auto build_level_order(const LevelOrderShape &shape, std::span<const DictUnit> values) -> void {
        assert_check([&] { return level_order_shape_error(shape).empty(); }, "Trie: invalid level-order shape");
        assert_check(
            [&] {
                return std::cmp_equal(std::ranges::count_if(shape.edges, &LevelOrderEdge::has_value), values.size())
                       && std::ranges::all_of(values, &DictUnit::has_value);
            },
            "Trie: level-order values must match the valued edges");
        auto next = Trie{};
        next.build_level_order_storage(shape, values);
        *this = std::move(next);
    }

    /// Copy the complete storage, including runtime edits that have not been compacted.
    [[nodiscard]]
    auto clone() const -> Trie {
        return Trie{*this};
    }

    /// Find an exact key, returning its dictionary payload by value.
    [[nodiscard]]
    auto find(std::span<const Rune> key) const noexcept -> DictUnit {
        const auto *entry = find_entry(key);
        return entry ? entry->value : DictUnit{};
    }

    /// Check whether inserting key stays within the configured node id range.
    [[nodiscard]]
    auto has_capacity_for(std::span<const Rune> key) const noexcept -> bool {
        const auto count = new_node_count(key);
        const auto first_id = std::max(node_count_, size_t{1});
        return count == 0 || detail::size_below_capacity<TrieNodeId>(first_id + count - 1);
    }

    /// Insert or replace one key in place. Mutation needs exclusive access to the trie;
    /// it may rehash transitions and move compact child sets into the hash table.
    auto insert(std::span<const Rune> key, DictUnit value) -> void {
        assert_check([&] { return !key.empty() && value.has_value(); }, "Trie: invalid insertion");
        assert_check([&] { return has_capacity_for(key); }, "Trie: insertion exceeds the configured index range");
        if (empty()) {
            // A trie compacted after erasing every word regains its root node here.
            node_count_ = 1;
        }
        auto *entry = &ensure_root(key.front());
        for (size_t i = 1; i < key.size(); ++i) {
            entry = &ensure_child(*entry, key[i]);
        }
        entry->value = value;
    }

    /// Clear one key's value in place and report whether it existed. Nodes and child filters remain;
    /// a stale filter bit only costs an extra table lookup.
    auto erase(std::span<const Rune> key) -> bool {
        auto *entry = find_mutable_entry(key);
        if (entry == nullptr || !entry->value.has_value()) {
            return false;
        }
        entry->value = DictUnit{};
        return true;
    }

    /// Rebuild from the words that still have values: drop nodes left by erase and restore compact child
    /// sets moved to the hash table by insert. A failure leaves the current trie intact.
    auto compact() -> void {
        const auto words = live_words();
        build(words.keys, words.values);
    }

    // Visit real dictionary matches from one rune position in increasing end order.
    // The callback borrows each dictionary value; unknown runes do not produce matches.
    template <MatchVisitor Emit>
    auto for_each_match_from(std::span<const Rune> runes, RuneIndex begin, Emit &&emit) const
        noexcept(noexcept(emit(RuneIndex{}, std::declval<const DictUnit &>()))) -> void {
        assert(detail::size_fits_capacity<RuneIndex>(runes.size()));
        assert(begin <= runes.size());
        if (empty() || begin == runes.size()) {
            return;
        }

        const auto *entry = find_root(runes[static_cast<size_t>(begin)]);
        for (auto end = static_cast<size_t>(begin) + 1; entry; ++end) {
            if (entry->value.has_value()) {
                emit(static_cast<RuneIndex>(end), entry->value);
            }
            if (end == runes.size()) {
                break;
            }
            entry = find_child(*entry, runes[end]);
        }
    }

    /// Find all matching prefixes in the trie to build a flat compressed DAG.
    /// Returns a Dag containing offsets and edges.
    [[nodiscard]]
    auto find_dag(std::span<const Rune> sentence) const -> Dag {
        auto dag = Dag{};
        find_dag_into(sentence, dag);
        return dag;
    }

    // Replace the DAG while retaining storage for subsequent segments and calls.
    auto find_dag_into(std::span<const Rune> sentence, Dag &dag) const -> void {
        const auto n = sentence.size();
        assert_check([&] { return detail::size_fits_capacity<RuneIndex>(n) && n < std::numeric_limits<size_t>::max(); },
                     "Trie: sentence has {} runes, exceeding the supported DAG index range", n);
        dag.offsets.clear();
        dag.edges.clear();
        dag.offsets.reserve(n + 1);
        if (empty() || n == 0) {
            dag.offsets.resize(n + 1);
            return;
        }

        // Every rune contributes a single-rune edge, even when no dictionary word matches.
        dag.edges.reserve(n);

        // We accumulate the edges directly into a flat vector.
        for (size_t i = 0; i < n; ++i) {
            assert_check([&] { return detail::size_fits_capacity<DagOffset>(dag.edges.size()); },
                         "Trie: DAG edge count exceeds the supported offset range");
            dag.offsets.push_back(static_cast<DagOffset>(dag.edges.size()));

            // A single-rune edge always comes first; an unknown rune carries kMissingWordWeight.
            const auto *entry = find_root(sentence[i]);
            dag.edges.push_back({static_cast<RuneIndex>(i + 1), entry ? entry->value.weight : kMissingWordWeight});
            for (size_t j = i + 1; entry && j < n; ++j) {
                entry = find_child(*entry, sentence[j]);
                if (entry && entry->value.has_value()) {
                    dag.edges.push_back({static_cast<RuneIndex>(j + 1), entry->value.weight});
                }
            }
        }
        assert_check([&] { return detail::size_fits_capacity<DagOffset>(dag.edges.size()); },
                     "Trie: DAG edge count exceeds the supported offset range");
        dag.offsets.push_back(static_cast<DagOffset>(dag.edges.size()));
    }

    /// Return the total number of nodes in the trie (including the root).
    [[nodiscard]]
    auto node_count() const noexcept -> size_t {
        return node_count_;
    }

    /// Check whether the trie is empty (has not been built, or built with no keys).
    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return node_count_ == 0;
    }

    /// Reconstruct parent/depth information only when statistics are requested.
    /// Parent ids precede child ids, so a linear pass suffices without retained node objects.
    [[nodiscard]]
    auto collect_stats() const -> TrieStats {
        auto stats = TrieStats{};
        stats.node_count = node_count_;
        if (empty()) {
            return stats;
        }

        /// Temporary metadata for the cold statistics traversal.
        struct NodeInfo {
            size_t depth{0};
            size_t fanout{0};
            TrieNodeId parent{kAbsentNode};
        };
        auto nodes = std::vector<NodeInfo>(node_count_);
        for_each_edge([&](TrieNodeId parent, Rune, const Entry &entry) {
            assert_check([&] { return parent != kAbsentNode && parent < node_count_; },
                         "Trie: invalid statistics parent");
            assert_check([&] { return entry.child != kAbsentNode; }, "Trie: absent transition in statistics");
            ++nodes[static_cast<size_t>(parent)].fanout;
            ++stats.edge_count;
            stats.value_count += entry.value.has_value();
            if (entry.child == 0) {
                ++stats.lazy_edge_count;
                return;
            }
            assert_check([&] { return parent < entry.child && entry.child < node_count_; },
                         "Trie: child ids must follow their parents");
            auto &child = nodes[static_cast<size_t>(entry.child)];
            assert_check([&] { return child.parent == kAbsentNode; }, "Trie: statistics revisited a node");
            child.parent = parent;
        });
        stats.direct_root_edge_count = static_cast<size_t>(
            std::ranges::count_if(root_, [](const Entry &entry) { return entry.child != kAbsentNode; }));
        stats.hashed_edge_count = transitions_.size();
        // Every edge outside the direct root table and the hash table lives in compact storage.
        stats.compact_edge_count = stats.edge_count - stats.direct_root_edge_count - stats.hashed_edge_count;

        size_t total_depth = 0;
        size_t total_leaf_depth = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto &node = nodes[i];
            if (i > 0) {
                assert_check([&] { return node.parent != kAbsentNode && node.parent < i; },
                             "Trie: unreachable statistics node");
                node.depth = nodes[static_cast<size_t>(node.parent)].depth + 1;
            }
            total_depth += node.depth;
            if (node.fanout == 0) {
                ++stats.leaf_count;
                total_leaf_depth += node.depth;
            }
            stats.max_depth = std::max(stats.max_depth, node.depth);
            stats.max_fanout = std::max(stats.max_fanout, node.fanout);
            if (node.depth >= stats.depth_histogram.size()) {
                stats.depth_histogram.resize(node.depth + 1, 0);
            }
            ++stats.depth_histogram[node.depth];
            if (node.fanout >= stats.fanout_histogram.size()) {
                stats.fanout_histogram.resize(node.fanout + 1, 0);
            }
            ++stats.fanout_histogram[node.fanout];
        }
        stats.avg_depth = static_cast<double>(total_depth) / static_cast<double>(node_count_);
        if (stats.leaf_count > 0) {
            stats.avg_leaf_depth = static_cast<double>(total_leaf_depth) / static_cast<double>(stats.leaf_count);
        }
        const auto non_leaf_count = node_count_ - stats.leaf_count;
        if (non_leaf_count > 0) {
            stats.avg_fanout = static_cast<double>(stats.edge_count) / static_cast<double>(non_leaf_count);
        }
        stats.transition_capacity = transitions_.capacity();
        stats.transition_load_factor = transitions_.load_factor();
        stats.root_table_bytes = root_.capacity() * sizeof(Entry);
        if (stats.transition_capacity > 0) {
            // Include slot payloads, control bytes, and an upper bound for SIMD/padding overhead.
            stats.transition_table_bytes = stats.transition_capacity * (sizeof(Transitions::value_type) + 1) + 32;
        }
        stats.compact_table_bytes =
            compact_.capacity() * sizeof(CompactTransition) + compact_offsets_.capacity() * sizeof(size_t);
        stats.total_estimated_bytes = stats.root_table_bytes + stats.transition_table_bytes + stats.compact_table_bytes;
        return stats;
    }
};

} // namespace neo_cppjieba::detail
