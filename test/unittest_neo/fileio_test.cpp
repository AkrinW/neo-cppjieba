#include "../PlatformTestUtils.hpp"
#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/Separator.hpp"
#include "neo/util/FileIO.hpp"
#include "neo/util/FilePlatform.hpp"
#include "neo/util/StringUtil.hpp"

#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;
using namespace std::string_view_literals;

TEST(StringUtilTest, DecodeValueConsumesTheEntireInteger) {
    const auto invalid = std::array<std::string_view, 4>{"12abc", "12 ", "12.5", std::string_view{"12\0", 3}};
    for (const auto input : invalid) {
        EXPECT_THROW(decode_value<int>(input), LogConfig::Exception) << input;
    }
}

TEST(StringUtilTest, DecodeValueConsumesTheEntireFloat) {
    EXPECT_THROW(decode_value<double>("1.25x"), LogConfig::Exception);
}

TEST(StringUtilTest, DecodeValueRejectsEmptyInput) {
    EXPECT_THROW(decode_value<int>(std::string_view{}), LogConfig::Exception);
    EXPECT_THROW(decode_value<double>(""), LogConfig::Exception);
}

TEST(StringUtilTest, DecodeValueRejectsOutOfRangeInput) {
    EXPECT_THROW(decode_value<int64_t>("9223372036854775808"), LogConfig::Exception);
    EXPECT_THROW(decode_value<double>("1e10000"), LogConfig::Exception);
}

TEST(StringUtilTest, DecodeValueAcceptsCompleteNumbers) {
    EXPECT_EQ(decode_value<int>("-42"), -42);
    EXPECT_EQ(decode_value<uint64_t>("18446744073709551615"), std::numeric_limits<uint64_t>::max());
    EXPECT_DOUBLE_EQ(decode_value<double>("1.25e-2"), 0.0125);
}

TEST(StringUtilTest, EncodeValueRoundTripsNumericLimits) {
    for (const auto value : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
        EXPECT_EQ(decode_value<int64_t>(encode_value(value)), value);
    }
    for (const auto value : {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max()}) {
        EXPECT_DOUBLE_EQ(decode_value<double>(encode_value(value)), value);
    }
}

TEST(StringUtilTest, SeparatorIndicesCountRunesAndIncludeEndSentinel) {
    const auto text = std::u32string_view{U"𠮷，中 。"};
    const auto runes = std::span<const char32_t>{text};
    const auto expected = std::vector<RuneIndex>{1, 3, 4, 5};
    auto indices = std::vector<RuneIndex>{99};
    get_pre_filter_separators(runes, indices);
    EXPECT_EQ(indices, expected);
    EXPECT_EQ(get_pre_filter_separators(runes), expected);
}

TEST(StringUtilTest, EmptySeparatorInputResetsOutputToEndSentinel) {
    const auto runes = std::span<const char32_t>{};
    const auto expected = std::vector<RuneIndex>{0};
    auto indices = std::vector<RuneIndex>{1, 2, 3};
    get_pre_filter_separators(runes, indices);
    EXPECT_EQ(indices, expected);
    EXPECT_EQ(get_pre_filter_separators(runes), expected);
}

// The benchmark times three separator predicates; they must classify every rune alike to compare equal work.
TEST(StringUtilTest, SeparatorVariantsAgreeOnEveryRune) {
    for (auto rune = char32_t{0}; rune <= char32_t{0x110000}; ++rune) {
        const auto expected = is_default_separator(rune);
        ASSERT_EQ(is_default_separator_unlikely(rune), expected) << static_cast<std::uint32_t>(rune);
        ASSERT_EQ(is_default_separator_branchless(rune), expected) << static_cast<std::uint32_t>(rune);
    }
}

// ─── lines_view tests ────────────────────────────────────────────────────────

TEST(LinesViewTest, BasicLines) {
    auto content = std::string_view{"hello\nworld\nfoo"};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "hello");
    EXPECT_EQ(result[1], "world");
    EXPECT_EQ(result[2], "foo");
}

TEST(LinesViewTest, TrailingNewline) {
    // Trailing newline should NOT produce an extra empty element
    auto content = std::string_view{"a\nb\n"};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
}

TEST(LinesViewTest, WindowsLineEndings) {
    auto content = std::string_view{"hello\r\nworld\r\n"};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "hello");
    EXPECT_EQ(result[1], "world");
}

TEST(LinesViewTest, EmptyContent) {
    auto content = std::string_view{};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    EXPECT_TRUE(result.empty());
}

TEST(LinesViewTest, SingleLineNoNewline) {
    auto content = std::string_view{"single"};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "single");
}

TEST(LinesViewTest, EmptyLines) {
    // "a\n\nb\n\n" -> "a", "", "b", ""
    // The last "\n" at the very end produces an empty remaining, which means the "" before it
    // is emitted, then the truly-empty remaining triggers done.
    auto content = std::string_view{"a\n\nb\n\n"};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "");
    EXPECT_EQ(result[2], "b");
    EXPECT_EQ(result[3], "");
}

TEST(LinesViewTest, OnlyNewlines) {
    auto content = std::string_view{"\n\n"};
    auto result = std::vector<std::string_view>{};
    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        result.push_back(line);
    }
    // "\n\n" -> first advance: pos=0, current="", remaining="\n"
    //        -> second advance: pos=0, current="", remaining=""
    //        -> third advance: remaining.empty() -> done
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "");
    EXPECT_EQ(result[1], "");
}

TEST(LinesViewTest, PostfixIncrementReturnsThePreviousLine) {
    const auto lines = get_line_view(std::string_view{"first\nsecond\n"});
    auto it = lines.begin();
    const auto previous = it++;
    EXPECT_EQ(*previous, "first");
    EXPECT_EQ(*it, "second");
}

// ─── split_view tests ───────────────────────────────────────────────────────

TEST(SplitViewTest, BasicSplit) {
    auto line = std::string_view{"AT&T 3 nz"};
    auto result = std::vector<std::string_view>{};
    auto split = get_split_view(line, ' ');
    for (auto &&field : split) {
        result.push_back(field);
    }
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "AT&T");
    EXPECT_EQ(result[1], "3");
    EXPECT_EQ(result[2], "nz");
}

TEST(SplitViewTest, SingleElement) {
    auto line = std::string_view{"word"};
    auto result = std::vector<std::string_view>{};
    auto split = get_split_view(line, ' ');
    for (auto &&field : split) {
        result.push_back(field);
    }
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "word");
}

TEST(SplitViewTest, EmptyInput) {
    const auto *empty = "";
    auto line = std::string_view{empty};
    auto result = std::vector<std::string_view>{};
    auto split = get_split_view(line, ' ');
    for (auto &&field : split) {
        result.push_back(field);
    }
    // Splitting empty string yields one empty element (standard behavior)
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "");
}

TEST(SplitViewTest, ConsecutiveDelimiters) {
    auto line = std::string_view{"a,,b,,c"};
    auto result = std::vector<std::string_view>{};
    auto split = get_split_view(line, ',');
    for (auto &&field : split) {
        result.push_back(field);
    }
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "");
    EXPECT_EQ(result[2], "b");
    EXPECT_EQ(result[3], "");
    EXPECT_EQ(result[4], "c");
}

TEST(SplitViewTest, TrailingDelimiter) {
    auto line = std::string_view{"a,b,"};
    auto result = std::vector<std::string_view>{};
    auto split = get_split_view(line, ',');
    for (auto &&field : split) {
        result.push_back(field);
    }
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "");
}

TEST(SplitViewTest, DictLineFormat) {
    // Simulate a jieba dict line: "word freq tag"
    auto line = std::string_view{"你好 12345 v"};
    auto result = std::vector<std::string_view>{};
    auto split = get_split_view(line, ' ');
    for (auto &&field : split) {
        result.push_back(field);
    }
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "你好");
    EXPECT_EQ(result[1], "12345");
    EXPECT_EQ(result[2], "v");
}

TEST(SplitViewTest, PostfixIncrementReturnsThePreviousField) {
    const auto fields = get_split_view(std::string_view{"word 12 n"}, ' ');
    auto it = fields.begin();
    const auto previous = it++;
    EXPECT_EQ(*previous, "word");
    EXPECT_EQ(*it, "12");
}

// ─── FileBuffer tests ────────────────────────────────────────────────────────

namespace {

// Each file-buffer test owns an isolated directory, including files created before a failing assertion.
class FileBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = create_temp_directory("neo-file-buffer-");
    }

    void TearDown() override {
        if (!directory_.empty()) {
            auto error = std::error_code{};
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    [[nodiscard]] auto file_path(std::string_view name) const -> std::string {
        return path_to_utf8(directory_ / name);
    }

    void write_file(std::string_view name, std::string_view content) const {
        auto output = std::ofstream{directory_ / name, std::ios::binary};
        ASSERT_TRUE(output.is_open());
        output << content;
        output.close();
        ASSERT_TRUE(output.good());
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(FileBufferTest, OpenDictFile) {
    auto path = std::string(DICT_DIR) + "/jieba.dict.utf8";
    auto file = read_file(path);
    EXPECT_TRUE(file.is_open());
    EXPECT_GT(file.size(), 0u);
    // The dict file should start with a known entry
    auto content = file.content();
    EXPECT_FALSE(content.empty());
}

TEST_F(FileBufferTest, MoveSemantics) {
    auto path = std::string(DICT_DIR) + "/stop_words.utf8";
    auto file1 = read_file(path);
    auto size1 = file1.size();
    auto content1 = file1.content();

    // Move construct
    auto file2 = std::move(file1);
    EXPECT_EQ(file2.size(), size1);
    EXPECT_EQ(file2.content(), content1);
    // Moved-from should be empty
    EXPECT_EQ(file1.size(), 0u); // NOLINT(bugprone-use-after-move): checks the moved-from state.
    EXPECT_FALSE(file1.is_open());
}

TEST_F(FileBufferTest, EmptyFileRemainsOpen) {
    ASSERT_NO_FATAL_FAILURE(write_file("empty", ""));
    const auto file = read_file(file_path("empty"));
    EXPECT_TRUE(file.is_open());
    EXPECT_EQ(file.size(), 0u);
    EXPECT_TRUE(file.content().empty());
}

TEST_F(FileBufferTest, MissingFileReportsPathAndSystemError) {
    const auto path = file_path("missing");
    try {
        const auto file = read_file(path);
        FAIL() << "Expected opening a missing file to throw";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find(path), std::string_view::npos);
        EXPECT_NE(message.find(std::format("{}", platform::ErrorMessage{platform::ErrorCategory::Generic, ENOENT})),
                  std::string_view::npos);
    }
}

TEST(ErrorMessageTest, GenericCodeFormatsStandardErrorText) {
    const auto text = std::format("{}", platform::ErrorMessage{platform::ErrorCategory::Generic, ENOENT});
    const auto expected = std::error_code{ENOENT, std::generic_category()}.message();
    // Standard library category text may differ from the C library text in letter case.
    EXPECT_TRUE(std::ranges::equal(text, expected, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    })) << text;
}

TEST(ErrorMessageTest, SystemCodeFormatsSingleLineText) {
    const auto code = test_platform::missing_file_system_error();
    const auto text = std::format("{}", platform::ErrorMessage{platform::ErrorCategory::System, code});
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find_first_of("\r\n"), std::string::npos);
}

TEST(ErrorMessageTest, UnknownCodeStillFormatsText) {
    EXPECT_FALSE(std::format("{}", platform::ErrorMessage{platform::ErrorCategory::Generic, 987654}).empty());
}

TEST_F(FileBufferTest, RejectsEmbeddedNullInsteadOfOpeningPathPrefix) {
    ASSERT_NO_FATAL_FAILURE(write_file("original", "original content"));
    auto path = file_path("original");
    path.push_back('\0');
    path.append("suffix");
    EXPECT_THROW(read_file(path), LogConfig::Exception);
}

TEST_F(FileBufferTest, PreservesBinaryBytes) {
    const auto contents = std::string{"first\r\nsecond\x1a\0last", 19};
    ASSERT_NO_FATAL_FAILURE(write_file("binary", contents));
    const auto file = read_file(file_path("binary"));
    EXPECT_EQ(file.content(), contents);
    EXPECT_EQ(file.size(), contents.size());
}

TEST_F(FileBufferTest, ReadsUtf8FilePath) {
    const auto path = directory_ / std::filesystem::path{u8"词典𠮷.utf8"};
    {
        auto output = std::ofstream{path, std::ios::binary};
        ASSERT_TRUE(output.is_open());
        output << "中文词典\r\n";
        output.close();
        ASSERT_TRUE(output.good());
    }
    const auto file = read_file(path_to_utf8(path));
    EXPECT_EQ(file.content(), "中文词典\r\n");
}

TEST_F(FileBufferTest, RejectsDirectory) {
    EXPECT_THROW(read_file(path_to_utf8(directory_)), LogConfig::Exception);
}

TEST_F(FileBufferTest, RejectsInvalidUtf8Path) {
    test_platform::expect_invalid_utf8_path_rejected();
}

TEST_F(FileBufferTest, RejectingDirectoryClosesDescriptor) {
    test_platform::expect_rejected_directory_closes_descriptor(directory_);
}

TEST_F(FileBufferTest, RejectsFifoWithoutWaitingForWriter) {
    test_platform::expect_fifo_rejected_without_waiting(directory_ / "fifo");
}

TEST_F(FileBufferTest, OpenForReadingReportsMissingFileAsStatus) {
    const auto opened = platform::open_for_reading(platform::native_path(file_path("missing")));
    EXPECT_EQ(opened.fd, -1);
    EXPECT_FALSE(opened.status.succeeded);
    EXPECT_EQ(opened.status.error.category, platform::ErrorCategory::Generic);
    EXPECT_EQ(opened.status.error.code, ENOENT);
}

TEST_F(FileBufferTest, ReadSomeReportsEndOfFileAsZeroBytes) {
    ASSERT_NO_FATAL_FAILURE(write_file("empty", ""));
    const auto opened = platform::open_for_reading(platform::native_path(file_path("empty")));
    ASSERT_TRUE(opened.status.succeeded);
    auto buffer = std::array<char, 8>{};
    const auto result = platform::read_some(opened.fd, buffer);
    EXPECT_EQ(platform::close_file_descriptor(opened.fd), 0);
    EXPECT_TRUE(result.status.succeeded);
    EXPECT_EQ(result.bytes, 0u);
}

TEST_F(FileBufferTest, AtomicReplacementPreservesLoadedContent) {
    ASSERT_NO_FATAL_FAILURE(write_file("published", "original content"));
    const auto path = file_path("published");
    const auto original = read_file(path);

    ASSERT_NO_FATAL_FAILURE(write_file("replacement", "replacement with a different size"));
    std::filesystem::rename(directory_ / "replacement", directory_ / "published");

    EXPECT_EQ(original.content(), "original content");
    const auto replacement = read_file(path);
    EXPECT_EQ(replacement.content(), "replacement with a different size");
}

TEST_F(FileBufferTest, MoveAssignmentTransfersBufferAndEmptiesSource) {
    ASSERT_NO_FATAL_FAILURE(write_file("source", "source content"));
    ASSERT_NO_FATAL_FAILURE(write_file("destination", "old destination content"));
    auto source = read_file(file_path("source"));
    auto destination = read_file(file_path("destination"));
    const auto content = source.content();

    destination = std::move(source);

    EXPECT_TRUE(destination.is_open());
    EXPECT_EQ(destination.content(), "source content");
    EXPECT_EQ(destination.content().data(), content.data());
    EXPECT_FALSE(source.is_open()); // NOLINT(bugprone-use-after-move): checks the moved-from state.
    EXPECT_EQ(source.size(), 0u);
    EXPECT_TRUE(source.content().empty());
}

TEST_F(FileBufferTest, LoadedContentSurvivesExternalTruncation) {
    const auto expected = std::string(8192, 'x');
    ASSERT_NO_FATAL_FAILURE(write_file("truncate", expected));
    const auto path = file_path("truncate");

    // Isolate the old mmap SIGBUS regression so it cannot crash the whole test runner.
    EXPECT_EXIT(
        {
            test_platform::prepare_fileio_death_test();
            const auto file = read_file(path);
            const auto content = file.content();
            std::filesystem::resize_file(directory_ / "truncate", 0);
            std::_Exit(content == expected ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(FileBufferTest, LoadedContentSurvivesInPlaceOverwrite) {
    ASSERT_NO_FATAL_FAILURE(write_file("overwrite", "original"));
    const auto file = read_file(file_path("overwrite"));
    const auto content = file.content();

    ASSERT_NO_FATAL_FAILURE(write_file("overwrite", "modified"));

    EXPECT_EQ(content, "original");
    EXPECT_EQ(file.content(), "original");
}

TEST_F(FileBufferTest, TruncationDuringReadThrowsInsteadOfReturningPartialContent) {
    ASSERT_NO_FATAL_FAILURE(write_file("short-read", "12345678"));
    const auto path = file_path("short-read");
    const auto close_file = [](std::FILE *file) {
        std::fclose(file);
    };
    const auto input = std::unique_ptr<std::FILE, decltype(close_file)>{
        test_platform::open_binary_file(directory_ / "short-read", test_platform::FileMode::Read), close_file};
    ASSERT_NE(input, nullptr);

    // Simulate truncation after the original size was obtained, including a successful partial read before EOF.
    std::filesystem::resize_file(directory_ / "short-read", 3);
    auto destination = std::array<char, 8>{};
    try {
        detail::platform::read_file_contents(test_platform::file_descriptor(input.get()), std::span<char>{destination},
                                             path);
        FAIL() << "Expected a truncated file to fail the read";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find("unexpected EOF"), std::string_view::npos);
        EXPECT_NE(message.find("read 3 of 8 bytes"), std::string_view::npos);
        EXPECT_NE(message.find(path), std::string_view::npos);
    }
}

TEST_F(FileBufferTest, ReadFailureReportsPathAndSystemError) {
    const auto path = file_path("write-only");
    const auto close_file = [](std::FILE *file) {
        std::fclose(file);
    };
    const auto output = std::unique_ptr<std::FILE, decltype(close_file)>{
        test_platform::open_binary_file(directory_ / "write-only", test_platform::FileMode::Write), close_file};
    ASSERT_NE(output, nullptr);
    auto destination = std::array<char, 8>{};
    try {
        detail::platform::read_file_contents(test_platform::file_descriptor(output.get()), std::span<char>{destination},
                                             path);
        FAIL() << "Expected reading a write-only descriptor to fail";
    } catch (const LogConfig::Exception &error) {
        const auto message = std::string_view{error.what()};
        EXPECT_NE(message.find(path), std::string_view::npos);
        EXPECT_NE(message.find(test_platform::write_only_read_error().message()), std::string_view::npos);
    }
}

TEST_F(FileBufferTest, SuccessfulLoadClosesDescriptorBeforeReturning) {
    ASSERT_NO_FATAL_FAILURE(write_file("loaded", "content"));
    const auto path = file_path("loaded");
    const auto descriptor_before = detail::platform::open_file(path);
    ASSERT_NE(descriptor_before, -1);
    ASSERT_EQ(detail::platform::close_file_descriptor(descriptor_before), 0);

    const auto file = read_file(path);

    const auto descriptor_after = detail::platform::open_file(path);
    ASSERT_NE(descriptor_after, -1);
    EXPECT_EQ(descriptor_after, descriptor_before);
    EXPECT_EQ(detail::platform::close_file_descriptor(descriptor_after), 0);
    EXPECT_TRUE(file.is_open());
    EXPECT_EQ(file.content(), "content");
}

// ─── Integration test: FileBuffer + lines + split ────────────────────────────

TEST(FileIOIntegrationTest, ParseDictLines) {
    auto path = std::string(DICT_DIR) + "/jieba.dict.utf8";
    auto file = read_file(path);
    auto content = file.content();

    auto line_count = size_t{0};
    auto three_field_count = size_t{0};

    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        if (line.empty()) {
            continue;
        }
        ++line_count;

        auto field_count = size_t{0};
        auto split = get_split_view(line, ' ');
        for ([[maybe_unused]] auto &&field : split) {
            ++field_count;
        }
        if (field_count == 3) {
            ++three_field_count;
        }
    }

    // jieba.dict.utf8 has ~349K lines, all with 3 columns
    EXPECT_GT(line_count, 300000u);
    EXPECT_EQ(line_count, three_field_count);
}

TEST(FileIOIntegrationTest, ParseStopWords) {
    auto path = std::string(DICT_DIR) + "/stop_words.utf8";
    auto file = read_file(path);

    auto count = size_t{0};
    auto lines = get_line_view(file.content());
    for (auto &&line : lines) {
        if (!line.empty()) {
            ++count;
        }
    }
    EXPECT_GT(count, 1000u);
}

TEST(FileIOIntegrationTest, ZeroCopyVerification) {
    // Verify that string_views from lines/split point into the original file buffer
    auto path = std::string(DICT_DIR) + "/jieba.dict.utf8";
    auto file = read_file(path);
    auto content = file.content();

    auto lines = get_line_view(content);
    for (auto &&line : lines) {
        // Each line's data pointer must be within the owned buffer
        ASSERT_GE(line.data(), content.data());
        ASSERT_LE(line.data() + line.size(), content.data() + content.size());

        auto split = get_split_view(line, ' ');
        for (auto &&field : split) {
            // Each field's data pointer must also be within the owned buffer
            ASSERT_GE(field.data(), content.data());
            ASSERT_LE(field.data() + field.size(), content.data() + content.size());
        }
        break; // Just verify the first line is enough
    }
}

// ─── Atomic file replacement tests ───────────────────────────────────────────

namespace {

// Each replacement test owns an isolated directory, so leftover temporary files are visible.
class FileReplaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = create_temp_directory("neo-file-replace-");
    }

    void TearDown() override {
        if (!directory_.empty()) {
            auto error = std::error_code{};
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    [[nodiscard]] auto file_path(std::string_view name) const -> std::string {
        return path_to_utf8(directory_ / name);
    }

    [[nodiscard]] auto entries() const -> std::vector<std::string> {
        auto names = std::vector<std::string>{};
        for (const auto &entry : std::filesystem::directory_iterator{directory_}) {
            names.push_back(path_to_utf8(entry.path().filename()));
        }
        return names;
    }

    std::filesystem::path directory_;
};

auto bytes_of(std::string_view text) -> std::span<const std::byte> {
    return std::as_bytes(std::span{text});
}

auto read_back(const std::string &path) -> std::string {
    return std::string{read_file(path).content()};
}

} // namespace

TEST_F(FileReplaceTest, CreatesFileWithTheGivenBytes) {
    const auto path = file_path("new.bin");
    replace_file_atomically(path, bytes_of("snapshot\0bytes"sv));
    EXPECT_EQ(read_back(path), "snapshot\0bytes"sv);
}

TEST_F(FileReplaceTest, EmptyBytesCreateAnEmptyFile) {
    const auto path = file_path("empty.bin");
    replace_file_atomically(path, {});
    EXPECT_EQ(std::filesystem::file_size(directory_ / "empty.bin"), 0u);
}

TEST_F(FileReplaceTest, ReplacesExistingContentsAndLeavesNoTemporaryFile) {
    const auto path = file_path("snapshot.bin");
    replace_file_atomically(path, bytes_of("previous contents"));
    replace_file_atomically(path, bytes_of("new"));
    EXPECT_EQ(read_back(path), "new");
    EXPECT_EQ(entries(), (std::vector<std::string>{"snapshot.bin"}));
}

TEST_F(FileReplaceTest, ReaderOpenedBeforeReplacementKeepsPreviousContents) {
    if (!test_platform::replacement_keeps_open_readers()) {
        GTEST_SKIP() << "Windows cannot replace a file a stream holds open";
    }
    const auto path = file_path("snapshot.bin");
    replace_file_atomically(path, bytes_of("previous"));
    auto reader = std::ifstream{directory_ / "snapshot.bin", std::ios::binary};
    ASSERT_TRUE(reader.is_open());

    replace_file_atomically(path, bytes_of("replacement"));
    const auto contents = std::string{std::istreambuf_iterator<char>{reader}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(contents, "previous");
    EXPECT_EQ(read_back(path), "replacement");
}

TEST_F(FileReplaceTest, FailedReplacementKeepsTheTargetAndRemovesTheTemporaryFile) {
    // A nonempty directory cannot be replaced by a file, so the failure comes after the temporary is written.
    std::filesystem::create_directory(directory_ / "target");
    {
        auto inner = std::ofstream{directory_ / "target" / "kept.txt"};
        inner << "kept";
    }
    EXPECT_THROW(replace_file_atomically(file_path("target"), bytes_of("bytes")), LogConfig::Exception);
    EXPECT_TRUE(std::filesystem::is_directory(directory_ / "target"));
    EXPECT_TRUE(std::filesystem::exists(directory_ / "target" / "kept.txt"));
    EXPECT_EQ(entries(), (std::vector<std::string>{"target"}));
}

TEST_F(FileReplaceTest, RejectsMissingDirectory) {
    const auto path = path_to_utf8(directory_ / "missing" / "snapshot.bin");
    try {
        replace_file_atomically(path, bytes_of("bytes"));
        FAIL() << "Expected a replacement error";
    } catch (const LogConfig::Exception &error) {
        EXPECT_NE(std::string_view{error.what()}.find("failed to create a temporary file"), std::string_view::npos)
            << error.what();
    }
    EXPECT_TRUE(entries().empty());
}

TEST_F(FileReplaceTest, RejectsPathWithoutFileName) {
    EXPECT_THROW(replace_file_atomically(path_to_utf8(directory_) + "/", bytes_of("bytes")), LogConfig::Exception);
    EXPECT_THROW(replace_file_atomically("", bytes_of("bytes")), LogConfig::Exception);
    EXPECT_TRUE(entries().empty());
}

TEST_F(FileReplaceTest, RejectsEmbeddedNullByte) {
    const auto path = file_path("snap") + std::string{"\0shot", 5};
    EXPECT_THROW(replace_file_atomically(path, bytes_of("bytes")), LogConfig::Exception);
    EXPECT_TRUE(entries().empty());
}
