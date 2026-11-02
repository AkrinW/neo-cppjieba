#include "neo/Jieba.hpp"

#include "CApiTestSupport.hpp"
#include "neo_cppjieba.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace c_api_test {
namespace {

// Every edit runs on a clone from make_clone, so the base fixture's engine stays as loaded.
class CApiEditTest : public CApiTest {};

TEST_F(CApiEditTest, AddedWordIsSegmentedAndTaggedOnlyInTheEditedClone) {
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    const auto word = std::string_view{"杭研大厦"};
    const auto tag = c_tag("nz");
    ASSERT_EQ(neo_jieba_v1_engine_add_word(edited.get(), word.data(), word.size(), &tag, &error), NEO_JIEBA_V1_OK)
        << error_message();
    const auto sentence = std::string_view{"他来到了网易杭研大厦"};
    EXPECT_EQ(cut_words(edited.get(), sentence, NEO_JIEBA_V1_MIX),
              (std::vector<std::string>{"他", "来到", "了", "网易", "杭研大厦"}));
    EXPECT_EQ(cut_words(engine.get(), sentence, NEO_JIEBA_V1_MIX),
              (std::vector<std::string>{"他", "来到", "了", "网易", "杭研", "大厦"}));
    auto found = neo_jieba_pos_tag_v1{};
    ASSERT_EQ(neo_jieba_v1_lookup_tag(edited.get(), word.data(), word.size(), &found, &error), NEO_JIEBA_V1_OK);
    EXPECT_EQ(tag_text(found), "nz");
}

TEST_F(CApiEditTest, EditsMatchTheSameEditsThroughCpp) {
    auto reference = neo_cppjieba::Jieba{paths.dict_path, paths.model_path, paths.user_dict_path};
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    const auto added = std::string_view{"杭研大厦"};
    const auto weighted = std::string_view{"京都大学深造"};
    const auto removed_word = std::string_view{"计算所"};
    const auto noun = c_tag("nz");
    const auto untagged = c_tag("");
    ASSERT_EQ(neo_jieba_v1_engine_add_word(edited.get(), added.data(), added.size(), &noun, &error), NEO_JIEBA_V1_OK);
    ASSERT_EQ(
        neo_jieba_v1_engine_add_word_with_freq(edited.get(), weighted.data(), weighted.size(), 3, &untagged, &error),
        NEO_JIEBA_V1_OK);
    auto removed = uint32_t{0};
    ASSERT_EQ(neo_jieba_v1_engine_remove_word(edited.get(), removed_word.data(), removed_word.size(), &removed, &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(removed, 1U);
    ASSERT_EQ(neo_jieba_v1_engine_compact(edited.get(), &error), NEO_JIEBA_V1_OK);

    reference.add_word(added, neo_cppjieba::PosTag{"nz"});
    reference.add_word_with_freq(weighted, 3, neo_cppjieba::PosTag{});
    reference.remove_word(removed_word);
    reference.compact();
    for (const auto sentence : sentences) {
        SCOPED_TRACE(sentence);
        EXPECT_EQ(cut_words(edited.get(), sentence, NEO_JIEBA_V1_MIX),
                  reference.cut_strings(sentence, neo_cppjieba::CutMode::MIX));
    }
    for (const auto word : {added, weighted, removed_word}) {
        SCOPED_TRACE(word);
        auto found = neo_jieba_pos_tag_v1{};
        ASSERT_EQ(neo_jieba_v1_lookup_tag(edited.get(), word.data(), word.size(), &found, &error), NEO_JIEBA_V1_OK);
        const auto expected = reference.lookup_tag(word);
        EXPECT_EQ(tag_text(found), expected.to_string_view());
    }
}

TEST_F(CApiEditTest, RemoveWordReportsWhetherTheWordWasPresent) {
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    const auto word = std::string_view{"清华大学"};
    auto removed = uint32_t{7};
    ASSERT_EQ(neo_jieba_v1_engine_remove_word(edited.get(), word.data(), word.size(), &removed, &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(removed, 1U);
    ASSERT_EQ(neo_jieba_v1_engine_remove_word(edited.get(), word.data(), word.size(), &removed, &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(removed, 0U);
    const auto words = cut_words(edited.get(), "我来到北京清华大学", NEO_JIEBA_V1_MIX);
    EXPECT_EQ(std::ranges::find(words, word), words.end());
}

TEST_F(CApiEditTest, CompactKeepsEveryCutResult) {
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    const auto added = std::string_view{"杭研大厦"};
    const auto removed_word = std::string_view{"清华大学"};
    const auto tag = c_tag("nz");
    auto removed = uint32_t{0};
    ASSERT_EQ(neo_jieba_v1_engine_add_word(edited.get(), added.data(), added.size(), &tag, &error), NEO_JIEBA_V1_OK);
    ASSERT_EQ(neo_jieba_v1_engine_remove_word(edited.get(), removed_word.data(), removed_word.size(), &removed, &error),
              NEO_JIEBA_V1_OK);
    const auto before = cut_all(edited.get());
    ASSERT_EQ(neo_jieba_v1_engine_compact(edited.get(), &error), NEO_JIEBA_V1_OK);
    EXPECT_EQ(cut_all(edited.get()), before);
}

TEST_F(CApiEditTest, CloneSurvivesTheEngineItWasCopiedFrom) {
    auto first = make_clone();
    ASSERT_NE(first, nullptr);
    neo_jieba_engine_v1 *created = nullptr;
    ASSERT_EQ(neo_jieba_v1_engine_clone(first.get(), &created, &error), NEO_JIEBA_V1_OK);
    const auto second = EngineOwner{created, neo_jieba_v1_engine_destroy};
    first.reset();
    EXPECT_EQ(cut_all(second.get()), cut_all(engine.get()));
}

TEST_F(CApiEditTest, FourByteTagsRoundTripWithoutTerminator) {
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    const auto word = std::string_view{"杭研大厦"};
    const auto tag = c_tag("abcd");
    ASSERT_EQ(neo_jieba_v1_engine_add_word(edited.get(), word.data(), word.size(), &tag, &error), NEO_JIEBA_V1_OK);
    auto found = neo_jieba_pos_tag_v1{};
    ASSERT_EQ(neo_jieba_v1_lookup_tag(edited.get(), word.data(), word.size(), &found, &error), NEO_JIEBA_V1_OK);
    EXPECT_EQ(tag_text(found), "abcd");
}

TEST_F(CApiEditTest, EditArgumentsAreRejectedBeforeTheDictionaryChanges) {
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    auto *const target = edited.get();
    const auto word = std::string_view{"杭研大厦"};
    const auto tag = c_tag("n");
    auto unpadded = c_tag("n");
    unpadded.data[2] = 'x';
    EXPECT_EQ(neo_jieba_v1_engine_add_word(nullptr, word.data(), word.size(), &tag, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_add_word(target, nullptr, 1, &tag, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_add_word(target, word.data(), 0, &tag, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_add_word(target, word.data(), word.size(), nullptr, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_add_word(target, word.data(), word.size(), &unpadded, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_add_word(target, word.data(), neo_jieba_v1_max_input_bytes() + 1, &tag, &error),
              NEO_JIEBA_V1_LIMIT_EXCEEDED);
    EXPECT_EQ(neo_jieba_v1_engine_add_word_with_freq(target, word.data(), word.size(), 0, &tag, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_add_word_with_freq(target, word.data(), word.size(), -1, &tag, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_remove_word(target, word.data(), word.size(), nullptr, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    auto removed = uint32_t{1};
    EXPECT_EQ(neo_jieba_v1_engine_remove_word(target, word.data(), 0, &removed, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(removed, 0U);
    EXPECT_EQ(neo_jieba_v1_engine_compact(nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    neo_jieba_engine_v1 *copy = nullptr;
    EXPECT_EQ(neo_jieba_v1_engine_clone(nullptr, &copy, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(copy, nullptr);
    EXPECT_EQ(neo_jieba_v1_engine_clone(engine.get(), nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);

    const auto sentence = std::string_view{"他来到了网易杭研大厦"};
    EXPECT_EQ(cut_words(target, sentence, NEO_JIEBA_V1_MIX), cut_words(engine.get(), sentence, NEO_JIEBA_V1_MIX));
}

TEST_F(CApiEditTest, InvalidUtf8EditsFailWithoutChangingTheDictionary) {
    const auto edited = make_clone();
    ASSERT_NE(edited, nullptr);
    const auto tag = c_tag("n");
    const auto invalid = std::string_view{"中\xE4\xB8"};
    EXPECT_EQ(neo_jieba_v1_engine_add_word(edited.get(), invalid.data(), invalid.size(), &tag, &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_GT(error.message_size, 0U);
    auto removed = uint32_t{1};
    EXPECT_EQ(neo_jieba_v1_engine_remove_word(edited.get(), invalid.data(), invalid.size(), &removed, &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_EQ(removed, 0U);
    EXPECT_EQ(cut_all(edited.get()), cut_all(engine.get()));
}

TEST_F(CApiEditTest, ReadersOfAnEngineRunWhileItsCloneIsMadeAndEdited) {
    const auto *shared = engine.get();
    const auto expected = cut_all(shared);
    auto edited = EngineOwner{nullptr, neo_jieba_v1_engine_destroy};
    {
        // Cloning only reads the shared engine; every edit goes to the private copy.
        auto writer = std::jthread{[shared, &edited] {
            neo_jieba_engine_v1 *created = nullptr;
            neo_jieba_v1_engine_clone(shared, &created, nullptr);
            edited.reset(created);
            const auto word = std::string_view{"杭研大厦"};
            const auto tag = c_tag("nz");
            for (auto round = std::size_t{0}; round < rounds; ++round) {
                auto removed = uint32_t{0};
                neo_jieba_v1_engine_add_word(created, word.data(), word.size(), &tag, nullptr);
                neo_jieba_v1_engine_remove_word(created, word.data(), word.size(), &removed, nullptr);
            }
            neo_jieba_v1_engine_compact(created, nullptr);
        }};
        for (const auto &result : run_workers([shared] { return cut_all(shared); })) {
            EXPECT_EQ(result, expected);
        }
    }
    EXPECT_NE(edited, nullptr);
}

} // namespace
} // namespace c_api_test
