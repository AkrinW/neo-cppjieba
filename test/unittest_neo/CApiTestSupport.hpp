#pragma once

#include "neo_cppjieba.h"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

// Helpers shared by the C ABI test files: handle owners, buffer helpers, and a fixture that loads one engine.
namespace c_api_test {

inline constexpr auto paths = neo_jieba_paths_v1{DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8", ""};
using EngineOwner = std::unique_ptr<neo_jieba_engine_v1, decltype(&neo_jieba_v1_engine_destroy)>;
using TokensOwner = std::unique_ptr<neo_jieba_tokens_v1, decltype(&neo_jieba_v1_tokens_destroy)>;
using WorkspaceOwner = std::unique_ptr<neo_jieba_workspace_v1, decltype(&neo_jieba_v1_workspace_destroy)>;
using RangesOwner = std::unique_ptr<neo_jieba_ranges_v1, decltype(&neo_jieba_v1_ranges_destroy)>;

// Describe a path as a C source; the path text must outlive the call that reads it.
inline
auto file_source(std::string_view path) -> neo_jieba_source_v1 {
    return {NEO_JIEBA_V1_SOURCE_FILE, 0, path.data(), path.size()};
}

// Describe borrowed contents as a C source.
inline
auto memory_source(std::string_view text) -> neo_jieba_source_v1 {
    return {NEO_JIEBA_V1_SOURCE_MEMORY, 0, text.data(), text.size()};
}

// Load a repository file as the contents a memory source borrows.
inline
auto read_text(std::string_view path) -> std::string {
    auto input = std::ifstream{std::string{path}, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// A shared engine amortizes dictionary loading; every test owns its diagnostics and results.
class CApiTest : public testing::Test {
protected:
    static
    void SetUpTestSuite() {
        auto storage = std::array<char, 512>{};
        auto diagnostic = neo_jieba_error_buffer_v1{storage.data(), storage.size(), 0};
        neo_jieba_engine_v1 *created = nullptr;
        ASSERT_EQ(neo_jieba_v1_engine_create(&paths, &created, &diagnostic), NEO_JIEBA_V1_OK) << storage.data();
        engine.reset(created);
    }

    static
    void TearDownTestSuite() {
        engine.reset();
    }

    auto cut(std::string_view input, neo_jieba_mode_v1 mode) -> TokensOwner {
        neo_jieba_tokens_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_cut_utf8_owned(engine.get(), input.data(), input.size(), mode, &created, &error),
                  NEO_JIEBA_V1_OK)
            << error_storage.data();
        return TokensOwner{created, neo_jieba_v1_tokens_destroy};
    }

    auto borrow(const TokensOwner &tokens) -> neo_jieba_borrowed_tokens_v1 {
        auto data = neo_jieba_borrowed_tokens_v1{};
        EXPECT_EQ(neo_jieba_v1_tokens_borrow_data(tokens.get(), &data, &error), NEO_JIEBA_V1_OK);
        return data;
    }

    auto make_workspace() -> WorkspaceOwner {
        neo_jieba_workspace_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_workspace_create(&created, &error), NEO_JIEBA_V1_OK) << error_storage.data();
        return WorkspaceOwner{created, neo_jieba_v1_workspace_destroy};
    }

    auto make_ranges() -> RangesOwner {
        neo_jieba_ranges_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_ranges_create(&created, &error), NEO_JIEBA_V1_OK) << error_storage.data();
        return RangesOwner{created, neo_jieba_v1_ranges_destroy};
    }

    auto borrow_ranges(const RangesOwner &ranges) -> neo_jieba_borrowed_ranges_v1 {
        auto data = neo_jieba_borrowed_ranges_v1{};
        EXPECT_EQ(neo_jieba_v1_ranges_borrow_data(ranges.get(), &data, &error), NEO_JIEBA_V1_OK);
        return data;
    }

    [[nodiscard]]
    auto error_message() const -> std::string_view {
        return error_storage.data();
    }

    // Edits run on a clone so the shared engine stays as loaded for every other test.
    auto make_clone() -> EngineOwner {
        neo_jieba_engine_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_engine_clone(engine.get(), &created, &error), NEO_JIEBA_V1_OK) << error_message();
        return EngineOwner{created, neo_jieba_v1_engine_destroy};
    }

    inline static auto engine = EngineOwner{nullptr, neo_jieba_v1_engine_destroy};
    std::array<char, 512> error_storage{};
    neo_jieba_error_buffer_v1 error{error_storage.data(), error_storage.size(), 0};
};

// Copy words only in the test harness; the public result itself contains byte ranges.
inline
auto words(const neo_jieba_borrowed_tokens_v1 &data) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    result.reserve(data.count);
    for (uint64_t index = 0; index < data.count; ++index) {
        const auto &range = data.ranges[index];
        result.emplace_back(data.utf8_data + range.byte_offset, range.byte_length);
    }
    return result;
}

// Slice the caller's input by borrowed ranges, as a zero-copy caller does.
inline
auto words(std::string_view input, const neo_jieba_borrowed_ranges_v1 &data) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    result.reserve(data.count);
    for (uint64_t index = 0; index < data.count; ++index) {
        result.emplace_back(input.substr(data.ranges[index].byte_offset, data.ranges[index].byte_length));
    }
    return result;
}

// A tag's text stops at its NUL padding; a four-byte tag has none.
inline
auto tag_text(const neo_jieba_pos_tag_v1 &tag) -> std::string_view {
    return {std::ranges::begin(tag.data), std::ranges::find(tag.data, '\0')};
}

// Build a NUL-padded tag from at most four bytes of text.
inline
auto c_tag(std::string_view text) -> neo_jieba_pos_tag_v1 {
    auto tag = neo_jieba_pos_tag_v1{};
    std::ranges::copy(text.substr(0, sizeof(tag.data)), std::ranges::begin(tag.data));
    return tag;
}

// Enough workers and repetitions for ThreadSanitizer to observe overlapping calls.
inline constexpr auto worker_count = std::size_t{4};
inline constexpr auto rounds = std::size_t{8};

inline constexpr auto all_modes = std::array<neo_jieba_mode_v1, 7>{
    NEO_JIEBA_V1_MIX,           NEO_JIEBA_V1_MIX_NO_HMM, NEO_JIEBA_V1_FULL, NEO_JIEBA_V1_SEARCH,
    NEO_JIEBA_V1_SEARCH_NO_HMM, NEO_JIEBA_V1_HMM,        NEO_JIEBA_V1_MP};

inline constexpr auto sentences = std::array<std::string_view, 3>{
    "他来到了网易杭研大厦", "小明硕士毕业于中国科学院计算所，后在日本京都大学深造", "去医院做B超，编号123😀"};

// Each call owns its result and diagnostics, as concurrent callers must; a failed call yields no words.
inline
auto cut_words(const neo_jieba_engine_v1 *engine, std::string_view input, neo_jieba_mode_v1 mode)
    -> std::vector<std::string> {
    auto storage = std::array<char, 256>{};
    auto diagnostic = neo_jieba_error_buffer_v1{storage.data(), storage.size(), 0};
    neo_jieba_tokens_v1 *created = nullptr;
    if (neo_jieba_v1_cut_utf8_owned(engine, input.data(), input.size(), mode, &created, &diagnostic)
        != NEO_JIEBA_V1_OK) {
        return {};
    }
    const auto owned = TokensOwner{created, neo_jieba_v1_tokens_destroy};
    auto data = neo_jieba_borrowed_tokens_v1{};
    if (neo_jieba_v1_tokens_borrow_data(owned.get(), &data, &diagnostic) != NEO_JIEBA_V1_OK) {
        return {};
    }
    return words(data);
}

// Cut every sentence in every mode, as one worker sharing the engine does.
inline
auto cut_all(const neo_jieba_engine_v1 *engine) -> std::vector<std::vector<std::string>> {
    auto results = std::vector<std::vector<std::string>>{};
    results.reserve(all_modes.size() * sentences.size());
    for (const auto mode : all_modes) {
        for (const auto sentence : sentences) {
            results.push_back(cut_words(engine, sentence, mode));
        }
    }
    return results;
}

// Run work repeatedly on worker_count threads at once; each thread keeps its last result.
template <typename Work>
auto run_workers(const Work &work) -> std::vector<std::invoke_result_t<const Work &>> {
    auto results = std::vector<std::invoke_result_t<const Work &>>(worker_count);
    {
        // Destroying the jthreads joins every worker before the results are read.
        auto workers = std::vector<std::jthread>{};
        workers.reserve(worker_count);
        for (auto &result : results) {
            workers.emplace_back([&work, &result] {
                for (auto round = std::size_t{0}; round < rounds; ++round) {
                    result = work();
                }
            });
        }
    }
    return results;
}

} // namespace c_api_test
