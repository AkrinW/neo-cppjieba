#pragma once

#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/Trie.hpp"
#include "neo/util/ByteOrder.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/Xxh64.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/SnapshotSource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace neo_cppjieba::detail {

// A snapshot stores the canonical level-order dictionary, never runtime trie storage, so it stays readable
// across storage changes, configurations and architectures. docs/snapshot.md has the layout and evolution rules.
static_assert(sizeof(int) == sizeof(std::int32_t), "DictSnapshot stores frequencies as 32-bit integers");

// Every format version begins with magic, format version and header size, so any reader can name a version it
// cannot read.
inline constexpr auto kSnapshotMagic = std::array{'N', 'E', 'O', 'J', 'B', 'S', 'N', 'P'};
inline constexpr auto kSnapshotPrefixSize = std::size_t{16};

// The format version this build writes; every earlier version keeps its case in decode_dict_snapshot.
inline constexpr auto kSnapshotFormatVersion = std::uint32_t{1};

// Format version 1 header and section table entry sizes, and its only checksum kind.
inline constexpr auto kSnapshotV1HeaderSize = std::size_t{48};
inline constexpr auto kSnapshotV1EntrySize = std::size_t{24};
inline constexpr auto kSnapshotV1ChecksumOffset = std::size_t{24};
inline constexpr auto kSnapshotChecksumXxh64 = std::uint32_t{1};

/// Section kinds keep their numbers forever; readers skip unknown optional kinds and reject unknown required ones.
enum class SnapshotSection : std::uint32_t { Meta = 1, Tags = 2, Trie = 3, UserSingleRunes = 4 };

// A section table flag: a reader that cannot decode a required section rejects the snapshot.
inline constexpr auto kSnapshotSectionRequired = std::uint16_t{1};

// Edge flags of trie section encoding 1; other bits are invalid.
inline constexpr auto kSnapshotEdgeChild = std::uint8_t{1};
inline constexpr auto kSnapshotEdgeValue = std::uint8_t{2};

// Section encodings this build decodes. Raising an encoding keeps the earlier ones listed and decodable.
[[nodiscard]] constexpr auto snapshot_encoding_supported(SnapshotSection kind, std::uint16_t encoding) noexcept
    -> bool {
    switch (kind) {
        case SnapshotSection::Meta:
        case SnapshotSection::Tags:
        case SnapshotSection::Trie:
        case SnapshotSection::UserSingleRunes: {
            return encoding == 1;
        }
        default: {
            return false;
        }
    }
}

// Content rules whose snapshots this build reads. Keep an older value only while text loading under the current
// rules would build the same dictionary from the same sources; otherwise its snapshots must be regenerated.
[[nodiscard]] constexpr auto snapshot_content_rules_supported(std::uint32_t rules) noexcept -> bool {
    switch (rules) {
        case kDictContentRules: {
            return true;
        }
        default: {
            return false;
        }
    }
}

// Format codes keep snapshots independent of the option enum's values.
[[nodiscard]] constexpr auto snapshot_option_code(UserWordWeightOption opt) noexcept -> std::uint8_t {
    switch (opt) {
        case UserWordWeightOption::WordWeightMin: {
            return 0;
        }
        case UserWordWeightOption::WordWeightMedian: {
            return 1;
        }
        case UserWordWeightOption::WordWeightMax: {
            return 2;
        }
        default: {
            assert_check([] { return false; }, "DictSnapshot: unchecked user word weight option {}",
                         static_cast<std::uint8_t>(opt));
            std::unreachable();
        }
    }
}

// Decode a stored option code; an unknown code is invalid snapshot data.
[[nodiscard]] inline auto snapshot_option(std::uint8_t code, std::string_view name) -> UserWordWeightOption {
    switch (code) {
        case 0: {
            return UserWordWeightOption::WordWeightMin;
        }
        case 1: {
            return UserWordWeightOption::WordWeightMedian;
        }
        case 2: {
            return UserWordWeightOption::WordWeightMax;
        }
        default: {
            break;
        }
    }
    check(false, "DictSnapshot: {} has unknown user word weight option code {}", name, static_cast<unsigned>(code));
    std::unreachable();
}

/// Reads the little-endian fields of one snapshot region in order; reading past its end is invalid data.
class SnapshotReader {
public:
    explicit SnapshotReader(std::span<const std::byte> bytes, std::string_view region, std::string_view name) noexcept
        : rest_{bytes}, region_{region}, name_{name} {
    }

    template <std::unsigned_integral T>
    [[nodiscard]] auto read() -> T {
        return load_le<T>(take(sizeof(T)));
    }

    [[nodiscard]] auto read_bytes(std::size_t count) -> std::span<const std::byte> {
        return take(count);
    }

    // Read an element count and reject one the remaining bytes cannot hold, before anything is allocated for it.
    [[nodiscard]] auto read_count(std::size_t element_size) -> std::size_t {
        const auto count = read<std::uint64_t>();
        check(count <= rest_.size() / element_size, "DictSnapshot: {} {} holds fewer bytes than its count {}", name_,
              region_, count);
        return static_cast<std::size_t>(count);
    }

    auto skip(std::size_t count) -> void {
        static_cast<void>(take(count));
    }

    auto expect_end() const -> void {
        check(rest_.empty(), "DictSnapshot: {} {} has {} unexpected trailing bytes", name_, region_, rest_.size());
    }

private:
    auto take(std::size_t count) -> std::span<const std::byte> {
        check(count <= rest_.size(), "DictSnapshot: {} {} ends early", name_, region_);
        const auto taken = rest_.first(count);
        rest_ = rest_.subspan(count);
        return taken;
    }

    std::span<const std::byte> rest_;
    std::string_view region_;
    std::string_view name_;
};

/// A located section payload with the encoding its writer used.
struct SnapshotSectionView {
    std::span<const std::byte> payload;
    std::uint16_t encoding{0};
    bool present{false};
};

/// The sections this build understands; absent ones stay empty.
struct SnapshotSections {
    SnapshotSectionView meta;
    SnapshotSectionView tags;
    SnapshotSectionView trie;
    SnapshotSectionView user_single_runes;
};

// The slot for a section kind this build understands, or null for an unknown kind.
[[nodiscard]] inline auto snapshot_section_slot(SnapshotSections &sections, std::uint32_t kind) noexcept
    -> SnapshotSectionView * {
    switch (static_cast<SnapshotSection>(kind)) {
        case SnapshotSection::Meta: {
            return &sections.meta;
        }
        case SnapshotSection::Tags: {
            return &sections.tags;
        }
        case SnapshotSection::Trie: {
            return &sections.trie;
        }
        case SnapshotSection::UserSingleRunes: {
            return &sections.user_single_runes;
        }
        default: {
            return nullptr;
        }
    }
}

// Record one table entry. A section this build cannot decode is skipped when optional and rejected when required.
inline auto add_snapshot_section(SnapshotSections &sections, std::span<const std::byte> entry,
                                 std::span<const std::byte> bytes, std::string_view name) -> void {
    auto reader = SnapshotReader{entry, "section table", name};
    const auto kind = reader.read<std::uint32_t>();
    const auto encoding = reader.read<std::uint16_t>();
    const auto flags = reader.read<std::uint16_t>();
    const auto offset = reader.read<std::uint64_t>();
    const auto size = reader.read<std::uint64_t>();
    check(offset <= bytes.size() && size <= bytes.size() - offset, "DictSnapshot: {} section {} lies outside the file",
          name, kind);
    auto *slot = snapshot_section_slot(sections, kind);
    if (slot == nullptr || !snapshot_encoding_supported(static_cast<SnapshotSection>(kind), encoding)) {
        check((flags & kSnapshotSectionRequired) == 0,
              "DictSnapshot: {} requires section {} encoding {}, which this build cannot decode; upgrade neo-cppjieba "
              "or regenerate the snapshot",
              name, kind, encoding);
        return;
    }
    check(!slot->present, "DictSnapshot: {} has more than one section {}", name, kind);
    *slot = SnapshotSectionView{bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size)),
                                encoding, true};
}

// Verify the checksum over everything after the header; every checksum kind a released build wrote stays listed.
inline auto check_snapshot_checksum(std::uint32_t kind, std::uint64_t expected, std::span<const std::byte> covered,
                                    std::string_view name) -> void {
    switch (kind) {
        case kSnapshotChecksumXxh64: {
            check(xxh64(covered, 0) == expected,
                  "DictSnapshot: {} fails its checksum; it is corrupt or was modified while being read", name);
            return;
        }
        default: {
            check(false, "DictSnapshot: {} uses checksum kind {}, which this build cannot verify", name, kind);
        }
    }
}

// Locate the sections of a format version 1 snapshot after checking its size, checksum and table bounds.
[[nodiscard]] inline auto locate_snapshot_v1_sections(std::span<const std::byte> bytes, std::string_view name)
    -> SnapshotSections {
    auto header = SnapshotReader{bytes, "header", name};
    header.skip(kSnapshotMagic.size() + sizeof(std::uint32_t));
    const auto header_size = header.read<std::uint32_t>();
    const auto file_size = header.read<std::uint64_t>();
    const auto checksum = header.read<std::uint64_t>();
    const auto checksum_kind = header.read<std::uint32_t>();
    const auto section_count = header.read<std::uint32_t>();
    const auto entry_size = header.read<std::uint32_t>();
    check(file_size == bytes.size(), "DictSnapshot: {} has {} bytes but records {}; it may be truncated", name,
          bytes.size(), file_size);
    check(header_size >= kSnapshotV1HeaderSize && header_size <= bytes.size(),
          "DictSnapshot: {} records an invalid header size {}", name, header_size);
    const auto body = bytes.subspan(header_size);
    check_snapshot_checksum(checksum_kind, checksum, body, name);
    check(entry_size >= kSnapshotV1EntrySize && section_count <= body.size() / entry_size,
          "DictSnapshot: {} has an invalid section table", name);

    auto sections = SnapshotSections{};
    for (auto i = std::size_t{0}; i < section_count; ++i) {
        add_snapshot_section(sections, body.subspan(i * entry_size, kSnapshotV1EntrySize), bytes, name);
    }
    return sections;
}

/// The fields of the meta section.
struct SnapshotMeta {
    FreqStats stats;
    UserWordWeightOption user_word_weight_opt{};
};

// Meta encoding 1: content rules, option code and the main-dictionary frequency statistics.
[[nodiscard]] inline auto decode_snapshot_meta_v1(std::span<const std::byte> payload, std::string_view name)
    -> SnapshotMeta {
    auto reader = SnapshotReader{payload, "meta section", name};
    const auto rules = reader.read<std::uint32_t>();
    check(snapshot_content_rules_supported(rules),
          "DictSnapshot: {} follows dictionary content rules {}, but this build applies rules {}; regenerate it from "
          "the source dictionaries",
          name, rules, kDictContentRules);
    auto meta = SnapshotMeta{};
    meta.user_word_weight_opt = snapshot_option(reader.read<std::uint8_t>(), name);
    meta.stats.sum = std::bit_cast<std::int64_t>(reader.read<std::uint64_t>());
    meta.stats.min = std::bit_cast<std::int32_t>(reader.read<std::uint32_t>());
    meta.stats.max = std::bit_cast<std::int32_t>(reader.read<std::uint32_t>());
    meta.stats.median = std::bit_cast<std::int32_t>(reader.read<std::uint32_t>());
    reader.expect_end();
    return meta;
}

// Tags encoding 1: a count, then each tag as a length byte and its bytes.
[[nodiscard]] inline auto decode_snapshot_tags_v1(std::span<const std::byte> payload, std::string_view name)
    -> std::vector<PosTag> {
    auto reader = SnapshotReader{payload, "tag section", name};
    const auto count = reader.read_count(1);
    auto tags = std::vector<PosTag>{};
    tags.reserve(count);
    for (auto i = std::size_t{0}; i < count; ++i) {
        const auto length = reader.read<std::uint8_t>();
        check(length <= PosTag::MaxLength, "DictSnapshot: {} has a {}-byte POS tag; at most {} are supported", name,
              static_cast<unsigned>(length), PosTag::MaxLength);
        const auto bytes = reader.read_bytes(length);
        tags.emplace_back(std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    }
    reader.expect_end();
    return tags;
}

/// The trie section: the level-order shape and its unweighted values.
struct SnapshotTrie {
    LevelOrderShape shape;
    std::vector<LevelOrderValue> values;
};

// Trie encoding 1: node, edge and value counts, then fanouts, edges as (rune, flags) and values as
// (frequency, tag index).
[[nodiscard]] inline auto decode_snapshot_trie_v1(std::span<const std::byte> payload, std::span<const PosTag> tags,
                                                  std::string_view name) -> SnapshotTrie {
    auto reader = SnapshotReader{payload, "trie section", name};
    const auto node_count = reader.read_count(sizeof(std::uint32_t));
    const auto edge_count = reader.read_count(sizeof(std::uint32_t) + 1);
    const auto value_count = reader.read_count(2 * sizeof(std::uint32_t));
    auto trie = SnapshotTrie{};
    trie.shape.fanouts.reserve(node_count);
    for (auto i = std::size_t{0}; i < node_count; ++i) {
        trie.shape.fanouts.push_back(reader.read<std::uint32_t>());
    }
    trie.shape.edges.reserve(edge_count);
    for (auto i = std::size_t{0}; i < edge_count; ++i) {
        const auto rune = static_cast<Rune>(reader.read<std::uint32_t>());
        const auto flags = reader.read<std::uint8_t>();
        check((flags & ~(kSnapshotEdgeChild | kSnapshotEdgeValue)) == 0, "DictSnapshot: {} has unknown edge flags {}",
              name, static_cast<unsigned>(flags));
        trie.shape.edges.push_back(
            LevelOrderEdge{rune, (flags & kSnapshotEdgeChild) != 0, (flags & kSnapshotEdgeValue) != 0});
    }
    trie.values.reserve(value_count);
    for (auto i = std::size_t{0}; i < value_count; ++i) {
        const auto freq = std::bit_cast<std::int32_t>(reader.read<std::uint32_t>());
        const auto tag = reader.read<std::uint32_t>();
        check(tag < tags.size(), "DictSnapshot: {} refers to tag {} of {}", name, tag, tags.size());
        trie.values.push_back(LevelOrderValue{freq, tags[tag]});
    }
    reader.expect_end();
    return trie;
}

// Single-rune user words encoding 1: a count, then the runes.
[[nodiscard]] inline auto decode_snapshot_user_single_runes_v1(std::span<const std::byte> payload,
                                                               std::string_view name) -> std::vector<Rune> {
    auto reader = SnapshotReader{payload, "single-rune user word section", name};
    const auto count = reader.read_count(sizeof(std::uint32_t));
    auto runes = std::vector<Rune>{};
    runes.reserve(count);
    for (auto i = std::size_t{0}; i < count; ++i) {
        runes.push_back(static_cast<Rune>(reader.read<std::uint32_t>()));
    }
    reader.expect_end();
    return runes;
}

// Dispatch each section to the decoder of its encoding; add_snapshot_section admitted only listed encodings.
[[nodiscard]] inline auto decode_snapshot_meta(const SnapshotSectionView &section, std::string_view name)
    -> SnapshotMeta {
    switch (section.encoding) {
        case 1: {
            return decode_snapshot_meta_v1(section.payload, name);
        }
        default: {
            assert_check([] { return false; }, "DictSnapshot: unlisted meta encoding");
            std::unreachable();
        }
    }
}

[[nodiscard]] inline auto decode_snapshot_tags(const SnapshotSectionView &section, std::string_view name)
    -> std::vector<PosTag> {
    switch (section.encoding) {
        case 1: {
            return decode_snapshot_tags_v1(section.payload, name);
        }
        default: {
            assert_check([] { return false; }, "DictSnapshot: unlisted tag encoding");
            std::unreachable();
        }
    }
}

[[nodiscard]] inline auto decode_snapshot_trie(const SnapshotSectionView &section, std::span<const PosTag> tags,
                                               std::string_view name) -> SnapshotTrie {
    switch (section.encoding) {
        case 1: {
            return decode_snapshot_trie_v1(section.payload, tags, name);
        }
        default: {
            assert_check([] { return false; }, "DictSnapshot: unlisted trie encoding");
            std::unreachable();
        }
    }
}

[[nodiscard]] inline auto decode_snapshot_user_single_runes(const SnapshotSectionView &section, std::string_view name)
    -> std::vector<Rune> {
    switch (section.encoding) {
        case 1: {
            return decode_snapshot_user_single_runes_v1(section.payload, name);
        }
        default: {
            assert_check([] { return false; }, "DictSnapshot: unlisted single-rune user word encoding");
            std::unreachable();
        }
    }
}

// Decode the sections into the canonical dictionary and validate it; format versions sharing these sections
// share this step.
[[nodiscard]] inline auto decode_snapshot_sections(const SnapshotSections &sections, std::string_view name)
    -> LevelOrderDict {
    check(sections.meta.present && sections.tags.present && sections.trie.present && sections.user_single_runes.present,
          "DictSnapshot: {} lacks a required section", name);
    const auto meta = decode_snapshot_meta(sections.meta, name);
    const auto tags = decode_snapshot_tags(sections.tags, name);
    auto trie = decode_snapshot_trie(sections.trie, tags, name);

    auto dict = LevelOrderDict{};
    dict.stats = meta.stats;
    dict.user_word_weight_opt = meta.user_word_weight_opt;
    dict.shape = std::move(trie.shape);
    dict.values = std::move(trie.values);
    dict.user_single_runes = decode_snapshot_user_single_runes(sections.user_single_runes, name);
    const auto error = level_order_dict_error(dict);
    check(error.empty(), "DictSnapshot: {} is invalid: {}", name, error);
    return dict;
}

/// Decode and validate snapshot bytes into the canonical dictionary; name labels diagnostics.
/// Every format version a released build wrote keeps its case here.
[[nodiscard]] inline auto decode_dict_snapshot(std::span<const std::byte> bytes, std::string_view name)
    -> LevelOrderDict {
    check(bytes.size() >= kSnapshotPrefixSize
              && std::ranges::equal(bytes.first(kSnapshotMagic.size()), std::as_bytes(std::span{kSnapshotMagic})),
          "DictSnapshot: {} is not a neo-cppjieba dictionary snapshot", name);
    const auto version = load_le<std::uint32_t>(bytes.subspan(kSnapshotMagic.size()));
    switch (version) {
        case 1: {
            return decode_snapshot_sections(locate_snapshot_v1_sections(bytes, name), name);
        }
        default: {
            break;
        }
    }
    check(false, "DictSnapshot: {} uses format version {}; this build reads versions 1 to {}", name, version,
          kSnapshotFormatVersion);
    std::unreachable();
}

/// One section ready for the table: its kind, encoding, flags and payload.
struct EncodedSnapshotSection {
    SnapshotSection kind;
    std::uint16_t encoding{0};
    std::uint16_t flags{0};
    std::vector<std::byte> payload;
};

// The distinct tags of the values in byte order; trie values refer to them by index.
[[nodiscard]] inline auto snapshot_tag_table(std::span<const LevelOrderValue> values) -> std::vector<PosTag> {
    auto tags = std::vector<PosTag>{};
    tags.reserve(values.size());
    std::ranges::transform(values, std::back_inserter(tags), &LevelOrderValue::tag);
    const auto text = [](const PosTag &tag) {
        return tag.to_string_view();
    };
    std::ranges::sort(tags, {}, text);
    const auto duplicates = std::ranges::unique(tags);
    tags.erase(duplicates.begin(), duplicates.end());
    return tags;
}

[[nodiscard]] inline auto encode_snapshot_meta_v1(const LevelOrderDict &dict) -> EncodedSnapshotSection {
    auto payload = std::vector<std::byte>{};
    append_le(payload, kDictContentRules);
    append_le(payload, snapshot_option_code(dict.user_word_weight_opt));
    append_le(payload, std::bit_cast<std::uint64_t>(dict.stats.sum));
    append_le(payload, std::bit_cast<std::uint32_t>(dict.stats.min));
    append_le(payload, std::bit_cast<std::uint32_t>(dict.stats.max));
    append_le(payload, std::bit_cast<std::uint32_t>(dict.stats.median));
    return {SnapshotSection::Meta, 1, kSnapshotSectionRequired, std::move(payload)};
}

[[nodiscard]] inline auto encode_snapshot_tags_v1(std::span<const PosTag> tags) -> EncodedSnapshotSection {
    auto payload = std::vector<std::byte>{};
    append_le(payload, static_cast<std::uint64_t>(tags.size()));
    for (const auto &tag : tags) {
        const auto text = tag.to_string_view();
        append_le(payload, static_cast<std::uint8_t>(text.size()));
        const auto bytes = std::as_bytes(std::span{text});
        payload.insert(payload.end(), bytes.begin(), bytes.end());
    }
    return {SnapshotSection::Tags, 1, kSnapshotSectionRequired, std::move(payload)};
}

[[nodiscard]] inline auto encode_snapshot_trie_v1(const LevelOrderDict &dict, std::span<const PosTag> tags)
    -> EncodedSnapshotSection {
    const auto &[fanouts, edges] = dict.shape;
    auto payload = std::vector<std::byte>{};
    payload.reserve(3 * sizeof(std::uint64_t) + fanouts.size() * sizeof(std::uint32_t)
                    + edges.size() * (sizeof(std::uint32_t) + 1) + dict.values.size() * 2 * sizeof(std::uint32_t));
    append_le(payload, static_cast<std::uint64_t>(fanouts.size()));
    append_le(payload, static_cast<std::uint64_t>(edges.size()));
    append_le(payload, static_cast<std::uint64_t>(dict.values.size()));
    for (const auto fanout : fanouts) {
        append_le(payload, fanout);
    }
    for (const auto &edge : edges) {
        append_le(payload, static_cast<std::uint32_t>(edge.rune));
        append_le(payload, static_cast<std::uint8_t>((edge.has_child ? kSnapshotEdgeChild : 0)
                                                     | (edge.has_value ? kSnapshotEdgeValue : 0)));
    }
    const auto text = [](const PosTag &tag) {
        return tag.to_string_view();
    };
    for (const auto &value : dict.values) {
        const auto tag = std::ranges::lower_bound(tags, value.tag.to_string_view(), {}, text);
        assert_check([&] { return tag != tags.end() && *tag == value.tag; }, "DictSnapshot: tag missing from table");
        append_le(payload, std::bit_cast<std::uint32_t>(value.freq));
        append_le(payload, static_cast<std::uint32_t>(tag - tags.begin()));
    }
    return {SnapshotSection::Trie, 1, kSnapshotSectionRequired, std::move(payload)};
}

[[nodiscard]] inline auto encode_snapshot_user_single_runes_v1(std::span<const Rune> runes) -> EncodedSnapshotSection {
    auto payload = std::vector<std::byte>{};
    payload.reserve(sizeof(std::uint64_t) + runes.size() * sizeof(std::uint32_t));
    append_le(payload, static_cast<std::uint64_t>(runes.size()));
    for (const auto rune : runes) {
        append_le(payload, static_cast<std::uint32_t>(rune));
    }
    return {SnapshotSection::UserSingleRunes, 1, kSnapshotSectionRequired, std::move(payload)};
}

// Lay out the format version 1 header, section table and payloads, then seal the checksum over everything after
// the header.
[[nodiscard]] inline auto assemble_snapshot_v1(std::span<const EncodedSnapshotSection> sections)
    -> std::vector<std::byte> {
    auto offset = kSnapshotV1HeaderSize + sections.size() * kSnapshotV1EntrySize;
    auto total = offset;
    for (const auto &section : sections) {
        total += section.payload.size();
    }
    auto bytes = std::vector<std::byte>{};
    bytes.reserve(total);
    const auto magic = std::as_bytes(std::span{kSnapshotMagic});
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    append_le(bytes, kSnapshotFormatVersion);
    append_le(bytes, static_cast<std::uint32_t>(kSnapshotV1HeaderSize));
    append_le(bytes, static_cast<std::uint64_t>(total));
    append_le(bytes, std::uint64_t{0}); // checksum, sealed below
    append_le(bytes, kSnapshotChecksumXxh64);
    append_le(bytes, static_cast<std::uint32_t>(sections.size()));
    append_le(bytes, static_cast<std::uint32_t>(kSnapshotV1EntrySize));
    append_le(bytes, std::uint32_t{0}); // reserved
    for (const auto &section : sections) {
        append_le(bytes, static_cast<std::uint32_t>(section.kind));
        append_le(bytes, section.encoding);
        append_le(bytes, section.flags);
        append_le(bytes, static_cast<std::uint64_t>(offset));
        append_le(bytes, static_cast<std::uint64_t>(section.payload.size()));
        offset += section.payload.size();
    }
    for (const auto &section : sections) {
        bytes.insert(bytes.end(), section.payload.begin(), section.payload.end());
    }
    assert_check([&] { return bytes.size() == total; }, "DictSnapshot: snapshot layout size mismatch");
    store_le(std::span{bytes}.subspan(kSnapshotV1ChecksumOffset),
             xxh64(std::span{bytes}.subspan(kSnapshotV1HeaderSize), 0));
    return bytes;
}

/// Encode a canonical dictionary in the current format version; equal dictionaries give equal bytes.
[[nodiscard]] inline auto encode_dict_snapshot(const LevelOrderDict &dict) -> std::vector<std::byte> {
    assert_check([&] { return level_order_dict_error(dict).empty(); },
                 "DictSnapshot: cannot encode an invalid level-order dictionary");
    const auto tags = snapshot_tag_table(dict.values);
    auto sections = std::vector<EncodedSnapshotSection>{};
    sections.reserve(4);
    sections.push_back(encode_snapshot_meta_v1(dict));
    sections.push_back(encode_snapshot_tags_v1(tags));
    sections.push_back(encode_snapshot_trie_v1(dict, tags));
    sections.push_back(encode_snapshot_user_single_runes_v1(dict.user_single_runes));
    return assemble_snapshot_v1(sections);
}

/// Build a dictionary from a snapshot. A file is read into owned memory before decoding, so later changes to it
/// cannot reach the dictionary; a change during the read fails the checksum.
[[nodiscard]] inline auto load_dict_snapshot(const SnapshotSource &source) -> DictTrie {
    return with_snapshot_bytes(source, [](std::span<const std::byte> bytes, std::string_view name) {
        return DictTrie{decode_dict_snapshot(bytes, name)};
    });
}

} // namespace neo_cppjieba::detail
