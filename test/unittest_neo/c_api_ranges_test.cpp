#include "neo/Jieba.hpp"

#include "CApiTestSupport.hpp"
#include "neo_cppjieba.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace c_api_test {
namespace {

// Join each word with its tag as "word/tag"; ranges without tags yield no words.
auto tagged_words(std::string_view input, const neo_jieba_borrowed_ranges_v1 &data) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    if (data.tags == nullptr) {
        return result;
    }
    result.reserve(data.count);
    for (uint64_t index = 0; index < data.count; ++index) {
        const auto &range = data.ranges[index];
        result.push_back(
            std::format("{}/{}", input.substr(range.byte_offset, range.byte_length), tag_text(data.tags[index])));
    }
    return result;
}

// Both C fill entry points share one signature, so the helpers below run either of them.
using FillRanges = decltype(&neo_jieba_v1_cut_utf8_into);
using ReadRanges = std::vector<std::string> (*)(std::string_view, const neo_jieba_borrowed_ranges_v1 &);

// A failed fill leaves the ranges empty, so it yields no words.
auto fill_words(const neo_jieba_engine_v1 *engine, std::string_view input, neo_jieba_mode_v1 mode,
                neo_jieba_workspace_v1 *workspace, neo_jieba_ranges_v1 *ranges, FillRanges fill, ReadRanges read)
    -> std::vector<std::string> {
    fill(engine, input.data(), input.size(), mode, workspace, ranges, nullptr);
    auto data = neo_jieba_borrowed_ranges_v1{};
    neo_jieba_v1_ranges_borrow_data(ranges, &data, nullptr);
    return read(input, data);
}

// One worker reuses its own workspace and ranges for every call; only the engine is shared.
auto fill_all(const neo_jieba_engine_v1 *engine, FillRanges fill, ReadRanges read)
    -> std::vector<std::vector<std::string>> {
    neo_jieba_workspace_v1 *created_workspace = nullptr;
    neo_jieba_ranges_v1 *created_ranges = nullptr;
    neo_jieba_v1_workspace_create(&created_workspace, nullptr);
    neo_jieba_v1_ranges_create(&created_ranges, nullptr);
    const auto workspace = WorkspaceOwner{created_workspace, neo_jieba_v1_workspace_destroy};
    const auto ranges = RangesOwner{created_ranges, neo_jieba_v1_ranges_destroy};
    auto results = std::vector<std::vector<std::string>>{};
    results.reserve(all_modes.size() * sentences.size());
    for (const auto mode : all_modes) {
        for (const auto sentence : sentences) {
            results.push_back(fill_words(engine, sentence, mode, workspace.get(), ranges.get(), fill, read));
        }
    }
    return results;
}

// Workspaces, ranges and tags reuse the base fixture's engine.
class CApiRangesTest : public CApiTest {};

TEST_F(CApiRangesTest, ConcurrentCutIntoWithSeparateHandlesMatchesSerialCuts) {
    const auto *shared = engine.get();
    const auto expected = cut_all(shared);

    for (const auto &result : run_workers([shared] { return fill_all(shared, neo_jieba_v1_cut_utf8_into, words); })) {
        EXPECT_EQ(result, expected);
    }
}

TEST_F(CApiRangesTest, CutIntoMatchesOwnedByteRangesInEveryMode) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    ASSERT_NE(workspace, nullptr);
    ASSERT_NE(ranges, nullptr);
    constexpr char text[] = "他来到了网易杭研大厦\0中国科学院😀𠀀";
    const auto input = std::string_view{text, sizeof(text) - 1};
    for (const auto mode : all_modes) {
        SCOPED_TRACE(mode);
        const auto owned = cut(input, mode);
        ASSERT_NE(owned, nullptr);
        const auto expected = borrow(owned);
        ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), input.data(), input.size(), mode, workspace.get(),
                                             ranges.get(), &error),
                  NEO_JIEBA_V1_OK)
            << error_storage.data();
        const auto data = borrow_ranges(ranges);
        ASSERT_EQ(data.count, expected.count);
        for (uint64_t index = 0; index < data.count; ++index) {
            EXPECT_EQ(data.ranges[index].byte_offset, expected.ranges[index].byte_offset);
            EXPECT_EQ(data.ranges[index].byte_length, expected.ranges[index].byte_length);
        }
    }
}

TEST_F(CApiRangesTest, CutIntoReplacesThePreviousFill) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    const auto first = std::string_view{"小明硕士毕业于中国科学院计算所"};
    const auto second = std::string_view{"中国科学院"};
    ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), first.data(), first.size(), NEO_JIEBA_V1_SEARCH, workspace.get(),
                                         ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), second.data(), second.size(), NEO_JIEBA_V1_MIX, workspace.get(),
                                         ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(words(second, borrow_ranges(ranges)), (std::vector<std::string>{"中国科学院"}));
}

TEST_F(CApiRangesTest, EmptyInputIntoProducesEmptyRangesInEveryMode) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    for (const auto mode : all_modes) {
        SCOPED_TRACE(mode);
        ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), nullptr, 0, mode, workspace.get(), ranges.get(), &error),
                  NEO_JIEBA_V1_OK);
        const auto data = borrow_ranges(ranges);
        EXPECT_EQ(data.ranges, nullptr);
        EXPECT_EQ(data.count, 0U);
    }
}

TEST_F(CApiRangesTest, RangesSurviveWorkspaceAndEngineDestruction) {
    const auto ranges = make_ranges();
    const auto input = std::string_view{"中国科学院"};
    {
        neo_jieba_engine_v1 *created = nullptr;
        ASSERT_EQ(neo_jieba_v1_engine_create(&paths, &created, &error), NEO_JIEBA_V1_OK);
        const auto local_engine = EngineOwner{created, neo_jieba_v1_engine_destroy};
        const auto workspace = make_workspace();
        ASSERT_EQ(neo_jieba_v1_cut_utf8_into(local_engine.get(), input.data(), input.size(), NEO_JIEBA_V1_MIX,
                                             workspace.get(), ranges.get(), &error),
                  NEO_JIEBA_V1_OK);
    }
    EXPECT_EQ(words(input, borrow_ranges(ranges)), (std::vector<std::string>{"中国科学院"}));
}

TEST_F(CApiRangesTest, FailedCutIntoLeavesRangesEmpty) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    const auto valid = std::string_view{"中国科学院"};
    ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), valid.data(), valid.size(), NEO_JIEBA_V1_MIX, workspace.get(),
                                         ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto invalid = std::string_view{"中国\xE4\xB8"};
    EXPECT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), invalid.data(), invalid.size(), NEO_JIEBA_V1_MIX,
                                         workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_GT(error.message_size, 0U);
    EXPECT_EQ(borrow_ranges(ranges).count, 0U);
}

TEST_F(CApiRangesTest, CutIntoRejectsMissingHandlesAndUnknownModes) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), "x", 1, NEO_JIEBA_V1_MIX, workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    EXPECT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), "x", 1, 0, workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(borrow_ranges(ranges).count, 0U);
    EXPECT_EQ(neo_jieba_v1_cut_utf8_into(nullptr, "x", 1, NEO_JIEBA_V1_MIX, workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(
        neo_jieba_v1_cut_utf8_into(engine.get(), nullptr, 1, NEO_JIEBA_V1_MIX, workspace.get(), ranges.get(), &error),
        NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), "x", 1, NEO_JIEBA_V1_MIX, nullptr, ranges.get(), &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), "x", 1, NEO_JIEBA_V1_MIX, workspace.get(), nullptr, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
}

TEST_F(CApiRangesTest, OversizeInputIntoIsRejectedBeforeReadingItsBytes) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    EXPECT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), "x", neo_jieba_v1_max_input_bytes() + 1, NEO_JIEBA_V1_MP,
                                         workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_LIMIT_EXCEEDED);
    EXPECT_EQ(borrow_ranges(ranges).count, 0U);
}

TEST_F(CApiRangesTest, RangesBorrowRejectsMissingArgumentsAndZeroesOutput) {
    const auto ranges = make_ranges();
    const auto stale = neo_jieba_token_range_v1{1, 2};
    auto data = neo_jieba_borrowed_ranges_v1{&stale, 1, nullptr};
    EXPECT_EQ(neo_jieba_v1_ranges_borrow_data(nullptr, &data, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(data.ranges, nullptr);
    EXPECT_EQ(data.count, 0U);
    EXPECT_EQ(neo_jieba_v1_ranges_borrow_data(ranges.get(), nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
}

TEST_F(CApiRangesTest, HandleCreationRequiresOutputStorage) {
    EXPECT_EQ(neo_jieba_v1_workspace_create(nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_ranges_create(nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
}

TEST_F(CApiRangesTest, TaggedCutIntoTagsMixWordsLikeCppJieba) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    const auto input = std::string_view{"我来到北京清华大学"};
    ASSERT_EQ(neo_jieba_v1_cut_utf8_tagged_into(engine.get(), input.data(), input.size(), NEO_JIEBA_V1_MIX,
                                                workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_OK)
        << error_message();
    EXPECT_EQ(tagged_words(input, borrow_ranges(ranges)),
              (std::vector<std::string>{"我/r", "来到/v", "北京/ns", "清华大学/nt"}));
}

TEST_F(CApiRangesTest, TaggedCutIntoKeepsRangesAndTagsEachWordInEveryMode) {
    const auto reference = neo_cppjieba::Jieba{paths.dict_path, paths.model_path, paths.user_dict_path};
    const auto workspace = make_workspace();
    const auto tagged_ranges = make_ranges();
    const auto plain_ranges = make_ranges();
    constexpr char text[] = "他来到了网易杭研大厦\0iPhone6 去医院做B超，编号123";
    const auto input = std::string_view{text, sizeof(text) - 1};
    for (const auto mode : all_modes) {
        SCOPED_TRACE(mode);
        ASSERT_EQ(neo_jieba_v1_cut_utf8_tagged_into(engine.get(), input.data(), input.size(), mode, workspace.get(),
                                                    tagged_ranges.get(), &error),
                  NEO_JIEBA_V1_OK);
        ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), input.data(), input.size(), mode, workspace.get(),
                                             plain_ranges.get(), &error),
                  NEO_JIEBA_V1_OK);
        const auto tagged = borrow_ranges(tagged_ranges);
        const auto plain = borrow_ranges(plain_ranges);
        ASSERT_EQ(tagged.count, plain.count);
        ASSERT_NE(tagged.tags, nullptr);
        for (uint64_t index = 0; index < tagged.count; ++index) {
            const auto &range = tagged.ranges[index];
            EXPECT_EQ(range.byte_offset, plain.ranges[index].byte_offset);
            EXPECT_EQ(range.byte_length, plain.ranges[index].byte_length);
            const auto expected = reference.lookup_tag(input.substr(range.byte_offset, range.byte_length));
            EXPECT_EQ(tag_text(tagged.tags[index]), expected.to_string_view());
        }
    }
}

TEST_F(CApiRangesTest, ConcurrentTaggedCutsWithSeparateHandlesMatchSerialTaggedCuts) {
    const auto *shared = engine.get();
    const auto expected = fill_all(shared, neo_jieba_v1_cut_utf8_tagged_into, tagged_words);
    ASSERT_TRUE(std::ranges::none_of(expected, [](const auto &tokens) { return tokens.empty(); }));

    for (const auto &result :
         run_workers([shared] { return fill_all(shared, neo_jieba_v1_cut_utf8_tagged_into, tagged_words); })) {
        EXPECT_EQ(result, expected);
    }
}

TEST_F(CApiRangesTest, UntaggedFillClearsTagsOfAnEarlierTaggedFill) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    const auto input = std::string_view{"中国科学院"};
    ASSERT_EQ(neo_jieba_v1_cut_utf8_tagged_into(engine.get(), input.data(), input.size(), NEO_JIEBA_V1_MIX,
                                                workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    ASSERT_NE(borrow_ranges(ranges).tags, nullptr);
    ASSERT_EQ(neo_jieba_v1_cut_utf8_into(engine.get(), input.data(), input.size(), NEO_JIEBA_V1_MIX, workspace.get(),
                                         ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto data = borrow_ranges(ranges);
    EXPECT_EQ(data.count, 1U);
    EXPECT_EQ(data.tags, nullptr);
}

TEST_F(CApiRangesTest, FailedTaggedCutIntoLeavesBothArraysEmpty) {
    const auto workspace = make_workspace();
    const auto ranges = make_ranges();
    const auto valid = std::string_view{"中国科学院"};
    ASSERT_EQ(neo_jieba_v1_cut_utf8_tagged_into(engine.get(), valid.data(), valid.size(), NEO_JIEBA_V1_MIX,
                                                workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_OK);
    const auto invalid = std::string_view{"中国\xE4\xB8"};
    EXPECT_EQ(neo_jieba_v1_cut_utf8_tagged_into(engine.get(), invalid.data(), invalid.size(), NEO_JIEBA_V1_MIX,
                                                workspace.get(), ranges.get(), &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    const auto data = borrow_ranges(ranges);
    EXPECT_EQ(data.ranges, nullptr);
    EXPECT_EQ(data.count, 0U);
    EXPECT_EQ(data.tags, nullptr);
}

TEST_F(CApiRangesTest, LookupTagUsesDictionaryTagsThenCppJiebaRule) {
    struct TagCase {
        std::string_view word;
        std::string_view tag;
    };
    for (const auto &[word, expected] : {TagCase{"拖拉机", "n"}, TagCase{"iPhone6", "eng"}, TagCase{"12345678", "m"},
                                         TagCase{"白富", "x"}, TagCase{"", "x"}}) {
        SCOPED_TRACE(word);
        auto tag = neo_jieba_pos_tag_v1{};
        ASSERT_EQ(neo_jieba_v1_lookup_tag(engine.get(), word.data(), word.size(), &tag, &error), NEO_JIEBA_V1_OK);
        EXPECT_EQ(tag_text(tag), expected);
    }
}

TEST_F(CApiRangesTest, LookupTagAcceptsNullEmptyWord) {
    auto tag = neo_jieba_pos_tag_v1{};
    ASSERT_EQ(neo_jieba_v1_lookup_tag(engine.get(), nullptr, 0, &tag, &error), NEO_JIEBA_V1_OK);
    EXPECT_EQ(tag_text(tag), "x");
}

TEST_F(CApiRangesTest, LookupTagRejectsMissingArgumentsAndOversizeWords) {
    auto tag = neo_jieba_pos_tag_v1{{'s', 't', 'a', 'l'}};
    EXPECT_EQ(neo_jieba_v1_lookup_tag(nullptr, "x", 1, &tag, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(tag_text(tag), "");
    EXPECT_EQ(neo_jieba_v1_lookup_tag(engine.get(), nullptr, 1, &tag, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_lookup_tag(engine.get(), "x", 1, nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_lookup_tag(engine.get(), "x", neo_jieba_v1_max_input_bytes() + 1, &tag, &error),
              NEO_JIEBA_V1_LIMIT_EXCEEDED);
}

TEST_F(CApiRangesTest, InvalidUtf8WordFailsAndZeroesTheTag) {
    auto tag = neo_jieba_pos_tag_v1{{'s', 't', 'a', 'l'}};
    const auto word = std::string_view{"中\xE4\xB8"};
    EXPECT_EQ(neo_jieba_v1_lookup_tag(engine.get(), word.data(), word.size(), &tag, &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_EQ(tag_text(tag), "");
    EXPECT_GT(error.message_size, 0U);
}

} // namespace
} // namespace c_api_test
