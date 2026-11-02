#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/KeywordExtractor.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMMState.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Unicode.hpp"

#include "TestUtils.hpp"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};
inline constexpr auto USER_DICT_FILE = std::string_view{DICT_DIR "/user.dict.utf8"};
inline constexpr auto TEST_USER_DICT_FILE = std::string_view{TEST_DATA_DIR "/userdict.utf8"};
inline constexpr auto IDF_FILE = std::string_view{DICT_DIR "/idf.utf8"};
inline constexpr auto STOP_WORDS_FILE = std::string_view{DICT_DIR "/stop_words.utf8"};

auto read_text(std::string_view path) -> std::string {
    auto input = std::ifstream{std::string{path}, std::ios::binary};
    EXPECT_TRUE(input.is_open()) << path;
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Read once and shared by the tests that load from memory.
auto dict_text() -> const std::string & {
    static const auto text = read_text(DICT_FILE);
    return text;
}

auto model_text() -> const std::string & {
    static const auto text = read_text(HMM_MODEL_FILE);
    return text;
}

auto file_jieba() -> const Jieba & {
    static const auto jieba = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    return jieba;
}

auto mix(const Jieba &jieba, std::string_view text, CutMode mode = CutMode::MIX) -> std::vector<std::string> {
    return jieba.cut_strings(text, mode);
}

constexpr auto sentences = std::array{
    "我来到北京清华大学"sv, "他来到了网易杭研大厦"sv,     "小明硕士毕业于中国科学院计算所，后在日本京都大学深造"sv,
    "南京市长江大桥"sv,     "令狐冲是云计算方面的专家"sv,
};
constexpr auto modes = std::array{CutMode::MIX,  CutMode::MIX_NO_HMM, CutMode::MP,           CutMode::HMM,
                                  CutMode::FULL, CutMode::SEARCH,     CutMode::SEARCH_NO_HMM};

// The bundled main dictionary as a snapshot, encoded once for the tests that only load it.
auto dict_snapshot() -> const std::vector<std::byte> & {
    static const auto bytes = Jieba::encode_dict_snapshot(DictSource::file(DICT_FILE), {});
    return bytes;
}

// Segmentation and tags must agree in every mode.
auto expect_same_cuts(const Jieba &actual, const Jieba &expected) -> void {
    for (const auto sentence : sentences) {
        for (const auto mode : modes) {
            EXPECT_EQ(actual.cut_strings<Tagging::Pos>(sentence, mode),
                      expected.cut_strings<Tagging::Pos>(sentence, mode))
                << sentence;
        }
    }
}

} // namespace

TEST(DictLoadingTest, MemorySourcesMatchFiles) {
    const auto user_text = read_text(USER_DICT_FILE);
    const auto from_files = Jieba{DICT_FILE, HMM_MODEL_FILE, USER_DICT_FILE};
    const auto user_dicts = std::array{DictSource::memory(user_text)};
    const auto from_memory =
        Jieba{DictSource::memory(dict_text()), DictSource::memory(model_text()), std::span{user_dicts}};
    for (const auto sentence : sentences) {
        for (const auto mode : modes) {
            EXPECT_EQ(mix(from_memory, sentence, mode), mix(from_files, sentence, mode)) << sentence;
        }
        EXPECT_EQ(from_memory.cut_strings<Tagging::Pos>(sentence, CutMode::MIX),
                  from_files.cut_strings<Tagging::Pos>(sentence, CutMode::MIX));
    }
}

TEST(DictLoadingTest, FileSourcesMatchPathConstructor) {
    const auto from_sources = Jieba{DictSource::file(DICT_FILE), DictSource::file(HMM_MODEL_FILE)};
    for (const auto sentence : sentences) {
        EXPECT_EQ(mix(from_sources, sentence), mix(file_jieba(), sentence));
    }
}

TEST(DictLoadingTest, HmmModelFromMemoryMatchesFile) {
    const auto from_file = HMModel{HMM_MODEL_FILE};
    const auto from_memory = HMModel{DictSource::memory(model_text())};
    for (const auto from : {HMMState::B, HMMState::E, HMMState::M, HMMState::S}) {
        EXPECT_EQ(from_memory.get_start_prob(from), from_file.get_start_prob(from));
        for (const auto to : {HMMState::B, HMMState::E, HMMState::M, HMMState::S}) {
            EXPECT_EQ(from_memory.get_trans_prob(from, to), from_file.get_trans_prob(from, to));
        }
    }
    for (const auto rune : decode("我的天𠀀a"sv)) {
        EXPECT_EQ(from_memory.get_emit_probs(rune), from_file.get_emit_probs(rune));
    }
}

TEST(DictLoadingTest, UserDictionariesLoadInOrderAndLaterEntriesWin) {
    const auto main = DictSource::memory("主词 10 n\n基础 20 n\n");
    const auto user_dicts = std::array{DictSource::memory("新词 v\n字\n"), DictSource::file(TEST_USER_DICT_FILE),
                                       DictSource::memory("新词 n\n")};
    const auto trie = DictTrie{main, user_dicts, UserWordWeightOption::WordWeightMedian};
    EXPECT_EQ(trie.find(decode("新词")).tag, PosTag{"n"});
    EXPECT_TRUE(trie.find(decode("云计算")).has_value());
    EXPECT_TRUE(trie.is_user_dict_single_chinese_word(decode("字")[0]));
    EXPECT_FALSE(trie.is_user_dict_single_chinese_word(decode("词")[0]));
}

TEST(DictLoadingTest, MemoryErrorsNameTheSource) {
    try {
        const auto trie =
            DictTrie{DictSource::memory("词 10\n", "embedded.dict"), {}, UserWordWeightOption::WordWeightMedian};
        FAIL() << "Expected a dictionary error";
    } catch (const LogConfig::Exception &error) {
        EXPECT_NE(std::string_view{error.what()}.find("embedded.dict"), std::string_view::npos) << error.what();
    }
    EXPECT_THROW((DictTrie{DictSource::memory(""), {}, UserWordWeightOption::WordWeightMedian}), LogConfig::Exception);
    EXPECT_THROW(HMModel{DictSource::memory("0 0 0 0\n")}, LogConfig::Exception);
}

TEST(DictLoadingTest, KeywordExtractorsLoadFromMemory) {
    const auto idf = read_text(IDF_FILE);
    const auto stop_words = read_text(STOP_WORDS_FILE);
    const auto &jieba = file_jieba();
    const auto text = "我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"sv;

    const auto tfidf_files = TfIdfExtractor{IDF_FILE, STOP_WORDS_FILE};
    const auto tfidf_memory = TfIdfExtractor{DictSource::memory(idf), DictSource::memory(stop_words)};
    const auto expected = tfidf_files.extract(jieba, text, 5, TfIdfOptions{});
    const auto actual = tfidf_memory.extract(jieba, text, 5, TfIdfOptions{});
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].word, expected[i].word);
        EXPECT_DOUBLE_EQ(actual[i].weight, expected[i].weight);
    }

    const auto textrank_files = TextRankExtractor{STOP_WORDS_FILE};
    const auto textrank_memory = TextRankExtractor{DictSource::memory(stop_words)};
    const auto expected_rank = textrank_files.extract(jieba, text, 5, TextRankOptions{});
    const auto actual_rank = textrank_memory.extract(jieba, text, 5, TextRankOptions{});
    ASSERT_EQ(actual_rank.size(), expected_rank.size());
    for (size_t i = 0; i < actual_rank.size(); ++i) {
        EXPECT_EQ(actual_rank[i].word, expected_rank[i].word);
        EXPECT_DOUBLE_EQ(actual_rank[i].weight, expected_rank[i].weight);
    }
}

TEST(DictLoadingTest, SnapshotCutsLikeTheSourcesItWasEncodedFrom) {
    const auto user_dicts = std::array{DictSource::file(USER_DICT_FILE), DictSource::file(TEST_USER_DICT_FILE)};
    const auto bytes = Jieba::encode_dict_snapshot(DictSource::file(DICT_FILE), user_dicts);
    const auto from_text = Jieba{DictSource::file(DICT_FILE), DictSource::file(HMM_MODEL_FILE), user_dicts};
    const auto from_snapshot =
        Jieba::from_snapshot(SnapshotSource::memory(bytes, "jieba.snapshot"), DictSource::file(HMM_MODEL_FILE));
    expect_same_cuts(from_snapshot, from_text);
}

TEST(DictLoadingTest, SnapshotFileLoadsLikeSnapshotMemory) {
    const auto directory = create_temp_directory("neo-jieba-snapshot-");
    const auto path = directory / "jieba.snapshot";
    {
        const auto &bytes = dict_snapshot();
        auto output = std::ofstream{path, std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto from_file =
        Jieba::from_snapshot(SnapshotSource::file(path_to_utf8(path)), DictSource::file(HMM_MODEL_FILE));
    std::filesystem::remove_all(directory);
    expect_same_cuts(from_file, file_jieba());
}

TEST(DictLoadingTest, SnapshotMemoryIsNotRetainedAfterLoading) {
    auto bytes = dict_snapshot();
    const auto jieba =
        Jieba::from_snapshot(SnapshotSource::memory(bytes, "jieba.snapshot"), DictSource::file(HMM_MODEL_FILE));
    std::ranges::fill(bytes, std::byte{0});
    bytes = {};
    expect_same_cuts(jieba, file_jieba());
}

TEST(DictLoadingTest, SnapshotJiebaAppliesRuntimeEditsLikeTextJieba) {
    auto from_text = Jieba{DICT_FILE, HMM_MODEL_FILE, ""};
    auto from_snapshot = Jieba::from_snapshot(SnapshotSource::memory(dict_snapshot(), "jieba.snapshot"),
                                              DictSource::file(HMM_MODEL_FILE));
    for (auto *jieba : {&from_text, &from_snapshot}) {
        jieba->add_word("杭研", PosTag{"nz"});
        jieba->add_word_with_freq("云计算方面", 500, PosTag{"n"});
        EXPECT_TRUE(jieba->remove_word("大厦"));
    }
    expect_same_cuts(from_snapshot, from_text);
    from_text.compact();
    from_snapshot.compact();
    expect_same_cuts(from_snapshot, from_text);
}

TEST(DictLoadingTest, SnapshotErrorsNameTheSource) {
    const auto garbage = std::array{std::byte{1}, std::byte{2}, std::byte{3}};
    try {
        const auto jieba =
            Jieba::from_snapshot(SnapshotSource::memory(garbage, "broken.snapshot"), DictSource::file(HMM_MODEL_FILE));
        FAIL() << "Expected a snapshot error";
    } catch (const LogConfig::Exception &error) {
        EXPECT_NE(std::string_view{error.what()}.find("broken.snapshot"), std::string_view::npos) << error.what();
    }
    // A text dictionary passed where a snapshot is expected is reported, not parsed.
    EXPECT_THROW(
        static_cast<void>(Jieba::from_snapshot(SnapshotSource::file(DICT_FILE), DictSource::file(HMM_MODEL_FILE))),
        LogConfig::Exception);
}

TEST(DictLoadingTest, WrittenSnapshotHoldsTheEncodedDictionaryAndLoads) {
    const auto directory = create_temp_directory("neo-jieba-snapshot-");
    const auto path = path_to_utf8(directory / "jieba.snapshot");
    Jieba::write_dict_snapshot(DictSource::file(DICT_FILE), {}, path);

    const auto written = read_text(path);
    const auto &expected = dict_snapshot();
    const auto jieba = Jieba::from_snapshot(SnapshotSource::file(path), DictSource::file(HMM_MODEL_FILE));
    std::filesystem::remove_all(directory);
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span{written}), expected));
    expect_same_cuts(jieba, file_jieba());
}
