#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/Dag.hpp"
#include "neo/resource/DictSnapshot.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/DictUnit.hpp"
#include "neo/resource/Trie.hpp"
#include "neo/util/ByteOrder.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/util/Xxh64.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/SnapshotSource.hpp"

#include "TestUtils.hpp"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;
using namespace std::string_view_literals;

namespace {

inline constexpr auto DICT_FILE = std::string_view{DICT_DIR "/jieba.dict.utf8"};
inline constexpr auto USER_DICT_FILE = std::string_view{DICT_DIR "/user.dict.utf8"};
inline constexpr auto TEST_USER_DICT_FILE = std::string_view{TEST_DATA_DIR "/userdict.utf8"};
inline constexpr auto CORPUS_FILE = std::string_view{TEST_DATA_DIR "/weicheng.utf8"};
inline constexpr auto GOLDEN_MAIN_DICT = std::string_view{TEST_DATA_DIR "/snapshot/main.dict.utf8"};
inline constexpr auto GOLDEN_USER_DICT = std::string_view{TEST_DATA_DIR "/snapshot/user.dict.utf8"};
// Encoded by format version 1 from the golden sources with WordWeightMedian. Once a release ships version 1 it is
// never regenerated, proving later builds still read it; before that release, v1 layout changes regenerate it.
inline constexpr auto GOLDEN_V1_SNAPSHOT = std::string_view{TEST_DATA_DIR "/snapshot/format_v1.bin"};

// Cover direct and hashed roots, a child set larger than compact storage, user words and unknown runes.
constexpr auto sentences =
    std::array{"我们在北京大学学中文"sv, "清华大学的学生"sv, "𠀀AT&T中午龘"sv, "云计算中心"sv, "中间的中国"sv};

auto read_text(std::string_view path) -> std::string {
    auto input = std::ifstream{std::string{path}, std::ios::binary};
    EXPECT_TRUE(input.is_open()) << path;
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Split text into its nonempty lines.
auto lines_of(std::string_view text) -> std::vector<std::string_view> {
    auto lines = std::vector<std::string_view>{};
    while (!text.empty()) {
        const auto end = std::min(text.find('\n'), text.size());
        if (end > 0) {
            lines.push_back(text.substr(0, end));
        }
        text.remove_prefix(std::min(end + 1, text.size()));
    }
    return lines;
}

// The first field of each nonempty line: every word a dictionary text defines.
auto words_of(std::string_view text) -> std::vector<std::string_view> {
    auto words = lines_of(text);
    std::ranges::transform(words, words.begin(), [](std::string_view line) { return line.substr(0, line.find(' ')); });
    return words;
}

auto same_dag(const Dag &actual, const Dag &expected) -> bool {
    return actual.offsets == expected.offsets
           && std::ranges::equal(actual.edges, expected.edges, [](const DagEdge &a, const DagEdge &b) {
                  return a.next_pos == b.next_pos && a.weight == b.weight;
              });
}

// Snapshot loading must be indistinguishable from text loading: every word, every DAG and every statistic.
auto expect_same_dictionary(const DictTrie &actual, const DictTrie &expected, std::span<const std::string_view> words,
                            std::span<const std::string_view> texts) -> void {
    EXPECT_EQ(actual.freq_sum(), expected.freq_sum());
    EXPECT_EQ(actual.min_weight(), expected.min_weight());
    EXPECT_EQ(actual.median_weight(), expected.median_weight());
    EXPECT_EQ(actual.max_weight(), expected.max_weight());
    EXPECT_EQ(actual.user_word_default_weight(), expected.user_word_default_weight());
    for (const auto word : words) {
        const auto runes = decode(word);
        const auto found = actual.find(runes);
        const auto wanted = expected.find(runes);
        EXPECT_EQ(found.weight, wanted.weight) << word;
        EXPECT_EQ(found.tag, wanted.tag) << word;
    }
    for (const auto text : texts) {
        const auto runes = decode(text);
        EXPECT_TRUE(same_dag(actual.find_dag(runes), expected.find_dag(runes))) << text;
        for (const auto rune : runes) {
            EXPECT_EQ(actual.is_user_dict_single_chinese_word(rune), expected.is_user_dict_single_chinese_word(rune))
                << text;
        }
    }
}

auto golden_user_dicts() -> std::array<DictSource, 1> {
    return {DictSource::file(GOLDEN_USER_DICT)};
}

auto golden_text_trie(UserWordWeightOption opt) -> DictTrie {
    const auto users = golden_user_dicts();
    return DictTrie{DictSource::file(GOLDEN_MAIN_DICT), users, opt};
}

auto golden_level_order(UserWordWeightOption opt) -> LevelOrderDict {
    const auto users = golden_user_dicts();
    return DictTrie::level_order(DictSource::file(GOLDEN_MAIN_DICT), users, opt);
}

auto golden_words() -> const std::vector<std::string_view> & {
    static const auto text = read_text(GOLDEN_MAIN_DICT) + read_text(GOLDEN_USER_DICT);
    static const auto words = words_of(text);
    return words;
}

auto runes_of(std::span<const std::string_view> words) -> std::vector<Unicode> {
    auto keys = std::vector<Unicode>{};
    std::ranges::transform(words, std::back_inserter(keys), [](std::string_view word) { return decode(word); });
    std::ranges::sort(keys);
    const auto duplicates = std::ranges::unique(keys);
    keys.erase(duplicates.begin(), duplicates.end());
    return keys;
}

/// One section of a crafted snapshot.
struct CraftedSection {
    std::uint32_t kind{0};
    std::uint16_t encoding{0};
    std::uint16_t flags{0};
    std::vector<std::byte> payload;
};

// Split a format version 1 snapshot into its sections, in table order.
auto split_sections(std::span<const std::byte> bytes) -> std::vector<CraftedSection> {
    const auto header_size = load_le<std::uint32_t>(bytes.subspan(12));
    const auto count = load_le<std::uint32_t>(bytes.subspan(36));
    const auto entry_size = load_le<std::uint32_t>(bytes.subspan(40));
    auto sections = std::vector<CraftedSection>{};
    sections.reserve(count);
    for (auto i = std::size_t{0}; i < count; ++i) {
        const auto entry = bytes.subspan(header_size + i * entry_size);
        const auto payload =
            bytes.subspan(load_le<std::uint64_t>(entry.subspan(8)), load_le<std::uint64_t>(entry.subspan(16)));
        sections.push_back(CraftedSection{load_le<std::uint32_t>(entry), load_le<std::uint16_t>(entry.subspan(4)),
                                          load_le<std::uint16_t>(entry.subspan(6)),
                                          std::vector<std::byte>{payload.begin(), payload.end()}});
    }
    return sections;
}

// Lay out sections behind a header of the given version and sizes, zero-filling the bytes version 1 leaves
// undefined, and seal a valid checksum so only the crafted difference is under test.
auto craft_snapshot(std::span<const CraftedSection> sections, std::uint32_t version, std::size_t header_size,
                    std::size_t entry_size) -> std::vector<std::byte> {
    auto bytes = std::vector<std::byte>(header_size + sections.size() * entry_size);
    std::ranges::copy(std::as_bytes(std::span{kSnapshotMagic}), bytes.begin());
    store_le(std::span{bytes}.subspan(8), version);
    store_le(std::span{bytes}.subspan(12), static_cast<std::uint32_t>(header_size));
    store_le(std::span{bytes}.subspan(32), kSnapshotChecksumXxh64);
    store_le(std::span{bytes}.subspan(36), static_cast<std::uint32_t>(sections.size()));
    store_le(std::span{bytes}.subspan(40), static_cast<std::uint32_t>(entry_size));
    for (auto i = std::size_t{0}; i < sections.size(); ++i) {
        const auto entry = std::span{bytes}.subspan(header_size + i * entry_size);
        store_le(entry, sections[i].kind);
        store_le(entry.subspan(4), sections[i].encoding);
        store_le(entry.subspan(6), sections[i].flags);
        store_le(entry.subspan(8), static_cast<std::uint64_t>(bytes.size()));
        store_le(entry.subspan(16), static_cast<std::uint64_t>(sections[i].payload.size()));
        bytes.insert(bytes.end(), sections[i].payload.begin(), sections[i].payload.end());
    }
    store_le(std::span{bytes}.subspan(16), static_cast<std::uint64_t>(bytes.size()));
    store_le(std::span{bytes}.subspan(kSnapshotV1ChecksumOffset), xxh64(std::span{bytes}.subspan(header_size), 0));
    return bytes;
}

auto reseal(std::span<const CraftedSection> sections) -> std::vector<std::byte> {
    return craft_snapshot(sections, 1, kSnapshotV1HeaderSize, kSnapshotV1EntrySize);
}

auto section_of(std::vector<CraftedSection> &sections, SnapshotSection kind) -> CraftedSection & {
    return *std::ranges::find(sections, static_cast<std::uint32_t>(kind), &CraftedSection::kind);
}

/// Byte offsets of the edge and value arrays in a trie section payload of encoding 1.
struct TrieLayout {
    std::size_t edges;
    std::size_t values;
};

auto trie_layout(std::span<const std::byte> payload) -> TrieLayout {
    const auto nodes = load_le<std::uint64_t>(payload);
    const auto edges = load_le<std::uint64_t>(payload.subspan(8));
    const auto edges_begin = 24 + nodes * sizeof(std::uint32_t);
    return {edges_begin, edges_begin + edges * (sizeof(std::uint32_t) + 1)};
}

// Three words over two nodes: the root's edges "a" and "b", then node 1's edge "b" spelling "ab".
auto tiny_snapshot() -> std::vector<std::byte> {
    return encode_dict_snapshot(DictTrie::level_order(DictSource::memory("a 1 n\nab 2 v\nb 3 n\n", "tiny"),
                                                      std::span<const DictSource>{},
                                                      UserWordWeightOption::WordWeightMedian));
}

// The tiny dictionary plus single-rune user words "c" and "d", stored in ascending order after a 64-bit count.
auto single_rune_snapshot() -> std::vector<std::byte> {
    const auto user_dicts = std::array{DictSource::memory("c\nd\n", "user")};
    return encode_dict_snapshot(DictTrie::level_order(DictSource::memory("a 1 n\nab 2 v\nb 3 n\n", "tiny"), user_dicts,
                                                      UserWordWeightOption::WordWeightMedian));
}

// Recompute the checksum after editing bytes it covers, so only the edit is under test.
auto reseal_checksum(std::vector<std::byte> &bytes) -> void {
    const auto header_size = load_le<std::uint32_t>(std::span{bytes}.subspan(12));
    store_le(std::span{bytes}.subspan(kSnapshotV1ChecksumOffset), xxh64(std::span{bytes}.subspan(header_size), 0));
}

// Expect decoding to fail through the checked path with a message naming the problem.
auto expect_rejected(std::span<const std::byte> bytes, std::string_view message) -> void {
    try {
        static_cast<void>(decode_dict_snapshot(bytes, "crafted.snapshot"));
        ADD_FAILURE() << "accepted a snapshot that should fail with: " << message;
    } catch (const LogConfig::Exception &error) {
        EXPECT_NE(std::string_view{error.what()}.find(message), std::string_view::npos) << error.what();
    }
}

} // namespace

TEST(SnapshotChecksumTest, MatchesReferenceXxh64) {
    const auto hash = [](std::string_view text) {
        return xxh64(std::as_bytes(std::span{text}), 0);
    };
    EXPECT_EQ(hash(""), 0xEF46DB3751D8E999);
    EXPECT_EQ(hash("a"), 0xD24EC4F1A98C6E5B);
    EXPECT_EQ(hash("abc"), 0x44BC2CF5AD770999ULL);
    // 39 bytes: one 32-byte stripe, then a 4-byte word and three single bytes.
    EXPECT_EQ(hash("Nobody inspects the spammish repetition"), 0xFBCEA83C8A378BF1);
}

TEST(LevelOrderTrieTest, NumbersNodesBreadthFirstWithSortedEdges) {
    const auto keys = std::vector<Unicode>{decode("a"), decode("ab"), decode("b"), decode("bcd")};
    const auto layout = level_order_keys(keys);

    EXPECT_EQ(layout.shape.fanouts, (std::vector<std::uint32_t>{2, 1, 1, 1}));
    EXPECT_EQ(
        layout.shape.edges,
        (std::vector<LevelOrderEdge>{
            {U'a', true, true}, {U'b', true, true}, {U'b', false, true}, {U'c', true, false}, {U'd', false, true}}));
    EXPECT_EQ(layout.value_keys, (std::vector<std::size_t>{0, 2, 1, 3}));
    EXPECT_TRUE(level_order_shape_error(layout.shape).empty());
}

TEST(LevelOrderTrieTest, RejectsNodeNotReachedFromEarlierNode) {
    const auto shape = LevelOrderShape{{1, 1}, {{U'a', false, true}, {U'b', true, true}}};
    EXPECT_EQ(level_order_shape_error(shape), "a trie node is not reached from an earlier node");
}

TEST(LevelOrderTrieTest, RejectsUnsortedRunesWithinNode) {
    const auto shape = LevelOrderShape{{2}, {{U'b', false, true}, {U'a', false, true}}};
    EXPECT_EQ(level_order_shape_error(shape), "edge runes are not strictly ascending within a node");
}

TEST(LevelOrderTrieTest, RejectsEdgeWithoutChildOrValue) {
    const auto shape = LevelOrderShape{{1}, {{U'a', false, false}}};
    EXPECT_EQ(level_order_shape_error(shape), "an edge has neither a child nor a value");
}

TEST(LevelOrderTrieTest, RejectsMoreChildEdgesThanNodes) {
    const auto shape = LevelOrderShape{{1}, {{U'a', true, true}}};
    EXPECT_EQ(level_order_shape_error(shape), "child edges do not match the node count");
}

TEST(LevelOrderTrieTest, RejectsSurrogateRune) {
    const auto shape = LevelOrderShape{{1}, {{static_cast<Rune>(0xD800), false, true}}};
    EXPECT_EQ(level_order_shape_error(shape), "an edge rune is not a Unicode scalar value");
}

TEST(LevelOrderTrieTest, LookupsMatchWordBuiltTrie) {
    const auto keys = runes_of(golden_words());
    auto values = std::vector<DictUnit>{};
    values.reserve(keys.size());
    for (auto i = std::size_t{0}; i < keys.size(); ++i) {
        values.push_back(DictUnit{-static_cast<WordWeight>(i + 1), PosTag{"n"}});
    }
    auto expected = Trie{};
    expected.build(keys, values);
    const auto layout = level_order_keys(keys);
    auto ordered = std::vector<DictUnit>{};
    std::ranges::transform(layout.value_keys, std::back_inserter(ordered),
                           [&](std::size_t key) { return values[key]; });
    auto actual = Trie{};
    actual.build_level_order(layout.shape, ordered);

    for (const auto &key : keys) {
        for (auto length = std::size_t{1}; length <= key.size(); ++length) {
            const auto prefix = std::span{key}.first(length);
            EXPECT_EQ(actual.find(prefix).weight, expected.find(prefix).weight);
        }
    }
    for (const auto sentence : sentences) {
        const auto runes = decode(sentence);
        EXPECT_TRUE(same_dag(actual.find_dag(runes), expected.find_dag(runes))) << sentence;
    }
}

TEST(LevelOrderTrieTest, PlacesEdgesInTheSameStorageAsWordBuild) {
    const auto keys = runes_of(golden_words());
    const auto values = std::vector<DictUnit>(keys.size(), DictUnit{-1.0, PosTag{"n"}});
    auto expected = Trie{};
    expected.build(keys, values);
    const auto layout = level_order_keys(keys);
    auto actual = Trie{};
    actual.build_level_order(layout.shape, std::span{values}.first(layout.value_keys.size()));

    const auto built = expected.collect_stats();
    const auto loaded = actual.collect_stats();
    EXPECT_EQ(loaded.node_count, built.node_count);
    EXPECT_EQ(loaded.direct_root_edge_count, built.direct_root_edge_count);
    EXPECT_EQ(loaded.hashed_edge_count, built.hashed_edge_count);
    EXPECT_EQ(loaded.compact_edge_count, built.compact_edge_count);
    EXPECT_EQ(loaded.total_estimated_bytes, built.total_estimated_bytes);
}

TEST(DictSnapshotTest, BundledDictionariesMatchTextLoading) {
    const auto users = std::array{DictSource::file(USER_DICT_FILE), DictSource::file(TEST_USER_DICT_FILE)};
    const auto dict = DictSource::file(DICT_FILE);
    const auto opt = UserWordWeightOption::WordWeightMedian;
    const auto bytes = encode_dict_snapshot(DictTrie::level_order(dict, users, opt));

    const auto text = read_text(DICT_FILE) + read_text(USER_DICT_FILE) + read_text(TEST_USER_DICT_FILE);
    const auto corpus = read_text(CORPUS_FILE);
    expect_same_dictionary(load_dict_snapshot(SnapshotSource::memory(bytes, "bundled")), DictTrie{dict, users, opt},
                           words_of(text), lines_of(corpus));
}

TEST(DictSnapshotTest, EveryUserWordWeightOptionMatchesTextLoading) {
    for (const auto opt : {UserWordWeightOption::WordWeightMin, UserWordWeightOption::WordWeightMedian,
                           UserWordWeightOption::WordWeightMax}) {
        const auto bytes = encode_dict_snapshot(golden_level_order(opt));
        expect_same_dictionary(load_dict_snapshot(SnapshotSource::memory(bytes, "golden")), golden_text_trie(opt),
                               golden_words(), sentences);
    }
}

TEST(DictSnapshotTest, DecodingReturnsTheEncodedDictionary) {
    const auto dict = golden_level_order(UserWordWeightOption::WordWeightMax);
    EXPECT_EQ(decode_dict_snapshot(encode_dict_snapshot(dict), "golden"), dict);
}

TEST(DictSnapshotTest, LineOrderOfDistinctWordsDoesNotChangeTheBytes) {
    const auto encode = [](std::string_view text) {
        return encode_dict_snapshot(DictTrie::level_order(
            DictSource::memory(text, "main"), std::span<const DictSource>{}, UserWordWeightOption::WordWeightMedian));
    };
    EXPECT_EQ(encode("北京 100 ns\n大学 80 n\n我们 200 r\n"), encode("我们 200 r\n北京 100 ns\n大学 80 n\n"));
}

TEST(DictSnapshotTest, LaterUserEntriesWinAsInTextLoading) {
    const auto main_dict = DictSource::memory("大学 80 n\n北京 100 ns\n", "main");
    const auto users = std::array{DictSource::memory("大学 5 x\n", "first"), DictSource::memory("大学 nt\n", "second")};
    const auto opt = UserWordWeightOption::WordWeightMin;
    const auto bytes = encode_dict_snapshot(DictTrie::level_order(main_dict, users, opt));
    const auto loaded = load_dict_snapshot(SnapshotSource::memory(bytes, "users"));
    const auto text = DictTrie{main_dict, users, opt};

    const auto word = decode("大学");
    EXPECT_EQ(loaded.find(word).tag, PosTag{"nt"});
    EXPECT_EQ(loaded.find(word).weight, text.find(word).weight);
}

TEST(DictSnapshotTest, FileLoadMatchesTextLoading) {
    const auto directory = create_temp_directory("neo-dict-snapshot-");
    const auto path = directory / "golden.snapshot";
    {
        const auto bytes = encode_dict_snapshot(golden_level_order(UserWordWeightOption::WordWeightMedian));
        auto output = std::ofstream{path, std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto loaded = load_dict_snapshot(SnapshotSource::file(path_to_utf8(path)));
    std::filesystem::remove_all(directory);
    expect_same_dictionary(loaded, golden_text_trie(UserWordWeightOption::WordWeightMedian), golden_words(), sentences);
}

TEST(DictSnapshotTest, FormatVersion1GoldenStillMatchesTextLoading) {
    expect_same_dictionary(load_dict_snapshot(SnapshotSource::file(GOLDEN_V1_SNAPSHOT)),
                           golden_text_trie(UserWordWeightOption::WordWeightMedian), golden_words(), sentences);
}

TEST(DictSnapshotTest, RejectsDataWithoutMagic) {
    auto bytes = tiny_snapshot();
    bytes.front() ^= std::byte{1};
    expect_rejected(bytes, "is not a neo-cppjieba dictionary snapshot");
    expect_rejected(std::span<const std::byte>{}, "is not a neo-cppjieba dictionary snapshot");
}

TEST(DictSnapshotTest, RejectsNewerFormatVersion) {
    const auto sections = split_sections(tiny_snapshot());
    expect_rejected(craft_snapshot(sections, 2, kSnapshotV1HeaderSize, kSnapshotV1EntrySize), "format version 2");
}

TEST(DictSnapshotTest, RejectsTruncatedSnapshot) {
    auto bytes = tiny_snapshot();
    bytes.pop_back();
    expect_rejected(bytes, "it may be truncated");
}

TEST(DictSnapshotTest, RejectsHeaderSizeOutsideTheFile) {
    const auto file_size = tiny_snapshot().size();
    for (const auto header_size : {kSnapshotV1HeaderSize - 1, file_size + 1}) {
        auto bytes = tiny_snapshot();
        store_le(std::span{bytes}.subspan(12), static_cast<std::uint32_t>(header_size));
        expect_rejected(bytes, "records an invalid header size");
    }
}

TEST(DictSnapshotTest, RejectsSectionOutsideTheFile) {
    const auto first_entry = kSnapshotV1HeaderSize;
    auto starts_past_end = tiny_snapshot();
    store_le(std::span{starts_past_end}.subspan(first_entry + 8),
             static_cast<std::uint64_t>(starts_past_end.size() + 1));
    reseal_checksum(starts_past_end);
    expect_rejected(starts_past_end, "lies outside the file");

    auto ends_past_end = tiny_snapshot();
    store_le(std::span{ends_past_end}.subspan(first_entry + 16), static_cast<std::uint64_t>(ends_past_end.size()));
    reseal_checksum(ends_past_end);
    expect_rejected(ends_past_end, "lies outside the file");
}

TEST(DictSnapshotTest, RejectsChecksumMismatch) {
    auto bytes = tiny_snapshot();
    bytes.back() ^= std::byte{1};
    expect_rejected(bytes, "fails its checksum");
}

TEST(DictSnapshotTest, RejectsUnknownRequiredSection) {
    auto sections = split_sections(tiny_snapshot());
    sections.push_back(CraftedSection{99, 1, kSnapshotSectionRequired, {}});
    expect_rejected(reseal(sections), "requires section 99 encoding 1");
}

TEST(DictSnapshotTest, SkipsUnknownOptionalSection) {
    const auto bytes = tiny_snapshot();
    auto sections = split_sections(bytes);
    sections.push_back(CraftedSection{99, 1, 0, {std::byte{7}}});
    EXPECT_EQ(decode_dict_snapshot(reseal(sections), "extended"), decode_dict_snapshot(bytes, "tiny"));
}

TEST(DictSnapshotTest, RejectsUnsupportedEncodingOfRequiredSection) {
    auto sections = split_sections(tiny_snapshot());
    section_of(sections, SnapshotSection::Trie).encoding = 2;
    expect_rejected(reseal(sections), "requires section 3 encoding 2");
}

TEST(DictSnapshotTest, RejectsDuplicateSection) {
    auto sections = split_sections(tiny_snapshot());
    const auto tags = section_of(sections, SnapshotSection::Tags);
    sections.push_back(tags);
    expect_rejected(reseal(sections), "more than one section 2");
}

TEST(DictSnapshotTest, RejectsMissingRequiredSection) {
    auto sections = split_sections(tiny_snapshot());
    std::erase_if(sections, [](const CraftedSection &section) {
        return section.kind == static_cast<std::uint32_t>(SnapshotSection::UserSingleRunes);
    });
    expect_rejected(reseal(sections), "lacks a required section");
}

TEST(DictSnapshotTest, IgnoresLongerHeaderAndTableEntries) {
    const auto bytes = tiny_snapshot();
    const auto extended =
        craft_snapshot(split_sections(bytes), 1, kSnapshotV1HeaderSize + 16, kSnapshotV1EntrySize + 8);
    EXPECT_EQ(decode_dict_snapshot(extended, "extended"), decode_dict_snapshot(bytes, "tiny"));
}

TEST(DictSnapshotTest, RejectsUnsupportedContentRules) {
    auto sections = split_sections(tiny_snapshot());
    store_le(std::span{section_of(sections, SnapshotSection::Meta).payload}, std::uint32_t{99});
    expect_rejected(reseal(sections), "follows dictionary content rules 99");
}

TEST(DictSnapshotTest, RejectsCountBeyondSectionSize) {
    auto sections = split_sections(tiny_snapshot());
    store_le(std::span{section_of(sections, SnapshotSection::Trie).payload}, std::uint64_t{1} << 40);
    expect_rejected(reseal(sections), "holds fewer bytes than its count");
}

TEST(DictSnapshotTest, RejectsInvalidTrieShape) {
    auto sections = split_sections(tiny_snapshot());
    auto &payload = section_of(sections, SnapshotSection::Trie).payload;
    payload[trie_layout(payload).edges + sizeof(std::uint32_t)] = std::byte{0};
    expect_rejected(reseal(sections), "an edge has neither a child nor a value");
}

TEST(DictSnapshotTest, RejectsUnknownEdgeFlags) {
    auto sections = split_sections(tiny_snapshot());
    auto &payload = section_of(sections, SnapshotSection::Trie).payload;
    payload[trie_layout(payload).edges + sizeof(std::uint32_t)] |= std::byte{0x80};
    expect_rejected(reseal(sections), "unknown edge flags");
}

TEST(DictSnapshotTest, RejectsTagIndexOutOfRange) {
    auto sections = split_sections(tiny_snapshot());
    auto &payload = section_of(sections, SnapshotSection::Trie).payload;
    store_le(std::span{payload}.subspan(trie_layout(payload).values + sizeof(std::uint32_t)), std::uint32_t{1000});
    expect_rejected(reseal(sections), "refers to tag 1000");
}

TEST(DictSnapshotTest, RejectsNegativeFrequency) {
    auto sections = split_sections(tiny_snapshot());
    auto &payload = section_of(sections, SnapshotSection::Trie).payload;
    store_le(std::span{payload}.subspan(trie_layout(payload).values), std::uint32_t{0xFFFFFFFF});
    expect_rejected(reseal(sections), "a word frequency is negative");
}

TEST(DictSnapshotTest, RejectsTrailingSectionBytes) {
    auto sections = split_sections(tiny_snapshot());
    section_of(sections, SnapshotSection::UserSingleRunes).payload.push_back(std::byte{0});
    expect_rejected(reseal(sections), "unexpected trailing bytes");
}

TEST(DictSnapshotTest, RejectsUnorderedSingleRuneUserWords) {
    auto sections = split_sections(single_rune_snapshot());
    auto &payload = section_of(sections, SnapshotSection::UserSingleRunes).payload;
    ASSERT_EQ(payload.size(), sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t));
    store_le(std::span{payload}.subspan(8), static_cast<std::uint32_t>(U'd'));
    store_le(std::span{payload}.subspan(12), static_cast<std::uint32_t>(U'c'));
    expect_rejected(reseal(sections), "single-rune user words are not ascending Unicode scalars");
}

TEST(DictSnapshotTest, RejectsSurrogateSingleRuneUserWord) {
    auto sections = split_sections(single_rune_snapshot());
    auto &payload = section_of(sections, SnapshotSection::UserSingleRunes).payload;
    ASSERT_EQ(payload.size(), sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t));
    store_le(std::span{payload}.subspan(8), std::uint32_t{0xD800});
    expect_rejected(reseal(sections), "single-rune user words are not ascending Unicode scalars");
}
