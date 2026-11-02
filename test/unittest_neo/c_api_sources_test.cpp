#include "neo/Jieba.hpp"

#include "CApiTestSupport.hpp"
#include "TestUtils.hpp"
#include "neo_cppjieba.h"
#include "test_paths.h"

#include <cstddef>
#include <filesystem>
#include <span>

#include <gtest/gtest.h>

namespace c_api_test {
namespace {

// Engines built from sources or snapshots, checked against the base fixture's file-loaded engine.
class CApiSourcesTest : public CApiTest {
protected:
    auto create_from_sources(const neo_jieba_source_v1 &dict, const neo_jieba_source_v1 &model,
                             std::span<const neo_jieba_source_v1> user_dicts) -> EngineOwner {
        neo_jieba_engine_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &model, user_dicts.data(), user_dicts.size(), &created,
                                                          &error),
                  NEO_JIEBA_V1_OK)
            << error_storage.data();
        return EngineOwner{created, neo_jieba_v1_engine_destroy};
    }

    auto create_from_snapshot(const neo_jieba_source_v1 &snapshot, const neo_jieba_source_v1 &model) -> EngineOwner {
        neo_jieba_engine_v1 *created = nullptr;
        EXPECT_EQ(neo_jieba_v1_engine_create_from_snapshot(&snapshot, &model, &created, &error), NEO_JIEBA_V1_OK)
            << error_storage.data();
        return EngineOwner{created, neo_jieba_v1_engine_destroy};
    }
};

TEST_F(CApiSourcesTest, MemorySourcesLoadLikeFilesAndAreNotRetained) {
    auto dict_text = read_text(paths.dict_path);
    auto model_text = read_text(paths.model_path);
    const auto from_memory = create_from_sources(memory_source(dict_text), memory_source(model_text), {});
    ASSERT_NE(from_memory, nullptr);
    dict_text.assign(dict_text.size(), 'x');
    model_text.assign(model_text.size(), 'x');
    EXPECT_EQ(cut_all(from_memory.get()), cut_all(engine.get()));
}

TEST_F(CApiSourcesTest, FileAndMemoryUserDictionariesMatchCppLoading) {
    const auto user_text = std::string{"杭研大厦\n"};
    const auto users = std::array{memory_source(user_text), file_source(TEST_DATA_DIR "/userdict.utf8")};
    const auto from_sources = create_from_sources(file_source(paths.dict_path), file_source(paths.model_path), users);
    ASSERT_NE(from_sources, nullptr);
    const auto cpp_users = std::array{neo_cppjieba::DictSource::memory(user_text),
                                      neo_cppjieba::DictSource::file(TEST_DATA_DIR "/userdict.utf8")};
    const auto reference = neo_cppjieba::Jieba{neo_cppjieba::DictSource::file(paths.dict_path),
                                               neo_cppjieba::DictSource::file(paths.model_path), cpp_users};
    for (const auto sentence : sentences) {
        SCOPED_TRACE(sentence);
        EXPECT_EQ(cut_words(from_sources.get(), sentence, NEO_JIEBA_V1_MIX),
                  reference.cut_strings(sentence, neo_cppjieba::CutMode::MIX));
    }
    EXPECT_NE(cut_words(from_sources.get(), sentences[0], NEO_JIEBA_V1_MIX),
              cut_words(engine.get(), sentences[0], NEO_JIEBA_V1_MIX));
}

TEST_F(CApiSourcesTest, WrittenSnapshotLoadsLikeItsSourcesFromFileOrMemory) {
    const auto user_text = std::string{"杭研大厦\n"};
    const auto dict = file_source(paths.dict_path);
    const auto model = file_source(paths.model_path);
    const auto users = std::array{memory_source(user_text)};
    const auto from_sources = create_from_sources(dict, model, users);
    ASSERT_NE(from_sources, nullptr);

    const auto directory = create_temp_directory("neo-jieba-c-snapshot-");
    const auto path = path_to_utf8(directory / "jieba.snapshot");
    ASSERT_EQ(neo_jieba_v1_write_dict_snapshot(&dict, users.data(), users.size(), path.data(), path.size(), &error),
              NEO_JIEBA_V1_OK)
        << error_message();
    const auto from_file = create_from_snapshot(file_source(path), model);
    const auto bytes = read_text(path);
    std::filesystem::remove_all(directory);
    const auto from_memory = create_from_snapshot(memory_source(bytes), model);
    ASSERT_NE(from_file, nullptr);
    ASSERT_NE(from_memory, nullptr);

    const auto expected = cut_all(from_sources.get());
    EXPECT_EQ(cut_all(from_file.get()), expected);
    EXPECT_EQ(cut_all(from_memory.get()), expected);
}

TEST_F(CApiSourcesTest, InvalidSourcesAreRejectedInEveryPositionBeforeLoading) {
    const auto dict = file_source(paths.dict_path);
    const auto model = file_source(paths.model_path);
    auto unknown_kind = dict;
    unknown_kind.kind = 0;
    auto nonzero_reserved = dict;
    nonzero_reserved.reserved = 1;
    constexpr char nul_path[] = DICT_DIR "/jieba.dict.utf8\0x";
    const auto invalid_sources = std::array{unknown_kind, nonzero_reserved, file_source(""),
                                            file_source(std::string_view{nul_path, sizeof(nul_path) - 1}),
                                            neo_jieba_source_v1{NEO_JIEBA_V1_SOURCE_MEMORY, 0, nullptr, 1}};
    for (auto index = std::size_t{0}; index < invalid_sources.size(); ++index) {
        SCOPED_TRACE(index);
        const auto &invalid = invalid_sources[index];
        neo_jieba_engine_v1 *result = nullptr;
        EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&invalid, &model, nullptr, 0, &result, &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
        EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &invalid, nullptr, 0, &result, &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
        EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &model, &invalid, 1, &result, &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
        EXPECT_EQ(neo_jieba_v1_engine_create_from_snapshot(&invalid, &model, &result, &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
        EXPECT_EQ(neo_jieba_v1_write_dict_snapshot(&invalid, nullptr, 0, "x", 1, &error),
                  NEO_JIEBA_V1_INVALID_ARGUMENT);
        EXPECT_EQ(result, nullptr);
    }
}

TEST_F(CApiSourcesTest, MissingSourceArgumentsAreRejected) {
    const auto dict = file_source(paths.dict_path);
    const auto model = file_source(paths.model_path);
    neo_jieba_engine_v1 *result = nullptr;
    EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(nullptr, &model, nullptr, 0, &result, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, nullptr, nullptr, 0, &result, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &model, nullptr, 1, &result, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &model, nullptr, 0, nullptr, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_create_from_snapshot(nullptr, &model, &result, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_engine_create_from_snapshot(&dict, &model, nullptr, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(result, nullptr);
}

TEST_F(CApiSourcesTest, SnapshotWriteRejectsInvalidPaths) {
    const auto dict = file_source(paths.dict_path);
    constexpr char nul_path[] = "jieba\0.snapshot";
    EXPECT_EQ(neo_jieba_v1_write_dict_snapshot(&dict, nullptr, 0, nullptr, 0, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_write_dict_snapshot(&dict, nullptr, 0, "", 0, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_write_dict_snapshot(&dict, nullptr, 0, nul_path, sizeof(nul_path) - 1, &error),
              NEO_JIEBA_V1_INVALID_ARGUMENT);
    EXPECT_EQ(neo_jieba_v1_write_dict_snapshot(&dict, nullptr, 1, "x", 1, &error), NEO_JIEBA_V1_INVALID_ARGUMENT);
}

TEST_F(CApiSourcesTest, MissingSourceFileIsNamedInTheError) {
    const auto dict = file_source(TEST_DATA_DIR "/c_api_nonexistent_file");
    const auto model = file_source(paths.model_path);
    neo_jieba_engine_v1 *result = nullptr;
    EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &model, nullptr, 0, &result, &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(error_message().find("c_api_nonexistent_file"), std::string_view::npos) << error_message();
}

TEST_F(CApiSourcesTest, MalformedMemorySourceIsNamedByItsRole) {
    const auto dict = file_source(paths.dict_path);
    const auto model = memory_source("not an hmm model");
    neo_jieba_engine_v1 *result = nullptr;
    EXPECT_EQ(neo_jieba_v1_engine_create_from_sources(&dict, &model, nullptr, 0, &result, &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_EQ(result, nullptr);
    EXPECT_NE(error_message().find("<memory:model>"), std::string_view::npos) << error_message();
}

TEST_F(CApiSourcesTest, CorruptSnapshotIsRejected) {
    const auto snapshot = memory_source("not a dictionary snapshot");
    const auto model = file_source(paths.model_path);
    neo_jieba_engine_v1 *result = nullptr;
    EXPECT_EQ(neo_jieba_v1_engine_create_from_snapshot(&snapshot, &model, &result, &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_EQ(result, nullptr);
    EXPECT_GT(error.message_size, 0U);
}

TEST_F(CApiSourcesTest, SnapshotWriteIntoMissingDirectoryFails) {
    const auto dict = memory_source("中国 3 ns\n");
    const auto path = std::string_view{TEST_DATA_DIR "/c_api_missing_directory/jieba.snapshot"};
    EXPECT_EQ(neo_jieba_v1_write_dict_snapshot(&dict, nullptr, 0, path.data(), path.size(), &error),
              NEO_JIEBA_V1_OPERATION_FAILED);
    EXPECT_GT(error.message_size, 0U);
    EXPECT_FALSE(std::filesystem::exists(TEST_DATA_DIR "/c_api_missing_directory"));
}

} // namespace
} // namespace c_api_test
