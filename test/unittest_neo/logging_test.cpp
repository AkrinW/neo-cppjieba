#include "../PlatformTestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/FilePlatform.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/Platform.hpp"

#include "test_paths.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
// detail::log stays qualified: a global using-declaration reads as the C library's log to include-cleaner.
using neo_cppjieba::detail::LogReceiver;
using neo_cppjieba::detail::LogRecordView;
using neo_cppjieba::detail::null_log_receiver;
using neo_cppjieba::detail::stderr_log_receiver;

// ─── LogLevel name mapping ──────────────────────────────────────────────────

TEST(LoggingTest, LogLevelNameDebug) {
    EXPECT_EQ(detail::log_level_name(LogLevel::LL_DEBUG), "DEBUG");
}

TEST(LoggingTest, LogLevelNameInfo) {
    EXPECT_EQ(detail::log_level_name(LogLevel::LL_INFO), "INFO");
}

TEST(LoggingTest, LogLevelNameWarning) {
    EXPECT_EQ(detail::log_level_name(LogLevel::LL_WARNING), "WARN");
}

TEST(LoggingTest, LogLevelNameError) {
    EXPECT_EQ(detail::log_level_name(LogLevel::LL_ERROR), "ERROR");
}

TEST(LoggingTest, LogLevelNameFatal) {
    EXPECT_EQ(detail::log_level_name(LogLevel::LL_FATAL), "FATAL");
}

// ─── LogLevel ordering ─────────────────────────────────────────────────────

TEST(LoggingTest, LogLevelOrdering) {
    EXPECT_LT(static_cast<uint8_t>(LogLevel::LL_DEBUG), static_cast<uint8_t>(LogLevel::LL_INFO));
    EXPECT_LT(static_cast<uint8_t>(LogLevel::LL_INFO), static_cast<uint8_t>(LogLevel::LL_WARNING));
    EXPECT_LT(static_cast<uint8_t>(LogLevel::LL_WARNING), static_cast<uint8_t>(LogLevel::LL_ERROR));
    EXPECT_LT(static_cast<uint8_t>(LogLevel::LL_ERROR), static_cast<uint8_t>(LogLevel::LL_FATAL));
}

// ─── LOG_LEVEL_ARRAY ────────────────────────────────────────────────────────

TEST(LoggingTest, LogLevelArraySize) {
    EXPECT_EQ(detail::LOG_LEVEL_ARRAY.size(), 5u);
}

TEST(LoggingTest, LogLevelArrayValues) {
    EXPECT_EQ(detail::LOG_LEVEL_ARRAY[0], "DEBUG");
    EXPECT_EQ(detail::LOG_LEVEL_ARRAY[1], "INFO");
    EXPECT_EQ(detail::LOG_LEVEL_ARRAY[2], "WARN");
    EXPECT_EQ(detail::LOG_LEVEL_ARRAY[3], "ERROR");
    EXPECT_EQ(detail::LOG_LEVEL_ARRAY[4], "FATAL");
}

// ─── LogLevel color mapping ─────────────────────────────────────────────────

TEST(LoggingTest, LogLevelColorArraySize) {
    EXPECT_EQ(detail::LOG_LEVEL_COLOR_ARRAY.size(), 5u);
}

TEST(LoggingTest, LogLevelColorDebugIsCyan) {
    EXPECT_EQ(detail::log_level_color(LogLevel::LL_DEBUG), "\033[36m");
}

TEST(LoggingTest, LogLevelColorInfoIsGreen) {
    EXPECT_EQ(detail::log_level_color(LogLevel::LL_INFO), "\033[32m");
}

TEST(LoggingTest, LogLevelColorWarningIsYellow) {
    EXPECT_EQ(detail::log_level_color(LogLevel::LL_WARNING), "\033[33m");
}

TEST(LoggingTest, LogLevelColorErrorIsRed) {
    EXPECT_EQ(detail::log_level_color(LogLevel::LL_ERROR), "\033[31m");
}

TEST(LoggingTest, LogLevelColorFatalIsBoldRed) {
    EXPECT_EQ(detail::log_level_color(LogLevel::LL_FATAL), "\033[1;31m");
}

TEST(LoggingTest, LogColorResetSequence) {
    EXPECT_EQ(detail::LOG_COLOR_RESET, "\033[0m");
}

// ─── Compile-time min level ─────────────────────────────────────────────────

TEST(LoggingTest, CompileTimeMinLevel) {
#ifndef NDEBUG
    EXPECT_EQ(detail::kCompileTimeMinLevel, LogLevel::LL_DEBUG);
#else
    EXPECT_EQ(detail::kCompileTimeMinLevel, LogLevel::LL_INFO);
#endif
}

// ─── cur_time returns non-empty formatted string ────────────────────────────

TEST(LoggingTest, CurTimeReturnsNonEmptyString) {
    auto buf = detail::cur_time();
    auto sv = std::string_view{buf.data()};
    EXPECT_FALSE(sv.empty());
    // Format: "YYYY-MM-DD HH:MM:SS" => at least 19 chars
    EXPECT_GE(sv.size(), 19u);
}

TEST(LoggingTest, CurTimeFormatPlausible) {
    auto buf = detail::cur_time();
    auto sv = std::string_view{buf.data()};
    // Should start with "20" (year 20xx)
    EXPECT_EQ(sv.substr(0, 2), "20");
    // Check separator positions: YYYY-MM-DD HH:MM:SS
    EXPECT_EQ(sv[4], '-');
    EXPECT_EQ(sv[7], '-');
    EXPECT_EQ(sv[10], ' ');
    EXPECT_EQ(sv[13], ':');
    EXPECT_EQ(sv[16], ':');
}

// ─── Constants ──────────────────────────────────────────────────────────────

TEST(LoggingTest, MaxTimeBufferSizeSufficient) {
    EXPECT_GE(detail::LOG_MAX_TIME_BUFFER_SIZE, 20u);
}

TEST(LoggingTest, MaxBufferSizeSufficient) {
    EXPECT_GE(detail::LOG_MAX_BUFFER_SIZE, 128u);
}

namespace {

// An application-owned exception distinct from the default runtime error.
class ProjectError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// Keep public diagnostics while selecting the application's exception type.
struct ProjectLogConfig {
    static constexpr auto show_source_location = true;
    static constexpr auto min_level = LogConfig::min_level;
    static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// Suppress source locations independently of the build mode and exception type.
struct PrivateLogConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = LogConfig::min_level;
    static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// An incomplete configuration must be rejected at the logging API boundary.
struct MissingExceptionConfig {
    [[maybe_unused]] static constexpr auto show_source_location = true;
    [[maybe_unused]] static constexpr auto min_level = LogConfig::min_level;
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
};

// An exception alias alone does not specify the source location policy.
struct MissingSourceLocationConfig {
    [[maybe_unused]] static constexpr auto min_level = LogConfig::min_level;
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// A constructible message type still needs to be an exception.
struct InvalidExceptionConfig {
    [[maybe_unused]] static constexpr auto show_source_location = true;
    [[maybe_unused]] static constexpr auto min_level = LogConfig::min_level;
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = std::string;
};

// An integer switch must not implicitly satisfy the boolean configuration contract.
struct IntegerSourceLocationConfig {
    static constexpr auto show_source_location = 1;
    [[maybe_unused]] static constexpr auto min_level = LogConfig::min_level;
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// Supply runtime-only initializers for negative compile-time configuration tests.
template <typename T>
auto runtime_config_value(T value) -> T {
    return value;
}

// An uninitialized constant declaration cannot supply a compile-time policy value.
struct NonConstantSourceLocationConfig {
    static const bool show_source_location;
    [[maybe_unused]] static constexpr auto min_level = LogConfig::min_level;
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): runtime initialization is what this config tests.
const bool NonConstantSourceLocationConfig::show_source_location =
    runtime_config_value(LogConfig::show_source_location);

// A high threshold filters ordinary messages without disabling failures.
struct HighThresholdLogConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = LogLevel::LL_FATAL;
    static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// A smaller record exercises bounded output with an explicitly enabled log level.
struct SmallBufferLogConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = LogLevel::LL_DEBUG;
    static constexpr auto buffer_size = std::size_t{128};
    using Exception = ProjectError;
};

// A one-byte record exercises truncation inside the formatted prefix.
struct OneByteBufferLogConfig : SmallBufferLogConfig {
    static constexpr auto buffer_size = std::size_t{1};
};

// A larger record preserves messages that exceed the default buffer capacity.
struct LargeBufferLogConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = LogLevel::LL_DEBUG;
    static constexpr auto buffer_size = std::size_t{1024};
    using Exception = ProjectError;
};

// An out-of-range threshold cannot name a supported severity.
struct InvalidLogLevelConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = static_cast<LogLevel>(255);
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// A runtime threshold cannot control compile-time filtering.
struct NonConstantLogLevelConfig {
    static constexpr auto show_source_location = false;
    static const LogLevel min_level;
    [[maybe_unused]] static constexpr auto buffer_size = LogConfig::buffer_size;
    using Exception = ProjectError;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): runtime initialization is what this config tests.
const LogLevel NonConstantLogLevelConfig::min_level = runtime_config_value(LogConfig::min_level);

// A log record needs at least one byte for its terminating newline.
struct EmptyBufferLogConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = LogConfig::min_level;
    static constexpr auto buffer_size = std::size_t{0};
    using Exception = ProjectError;
};

// A runtime capacity cannot size the stack-allocated log record.
struct NonConstantBufferLogConfig {
    static constexpr auto show_source_location = false;
    static constexpr auto min_level = LogConfig::min_level;
    static const std::size_t buffer_size;
    using Exception = ProjectError;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): runtime initialization is what this config tests.
const std::size_t NonConstantBufferLogConfig::buffer_size = runtime_config_value(LogConfig::buffer_size);

// Owned copy of a delivered record, because receivers only borrow the message.
struct CapturedRecord {
    LogLevel level;
    std::string message;
    std::source_location location;
};

// Receiver that copies each delivered record into a test-owned vector.
auto capture_records(std::vector<CapturedRecord> &records) -> LogReceiver {
    return LogReceiver{&records, [](void *context, const LogRecordView &record) noexcept {
                           static_cast<std::vector<CapturedRecord> *>(context)->push_back(
                               CapturedRecord{record.level, std::string{record.message}, record.location});
                       }};
}

// Assertion death tests verify the terminate handler is invoked without leaving core dumps.
auto prepare_assertion_death_test() -> void {
    std::set_terminate([] { std::_Exit(73); });
}

// Helper: capture stderr output from a callable.
auto capture_stderr(auto &&fn) -> std::string {
    // Flush stderr first
    std::fflush(stderr);

    // Create a temporary file to redirect stderr
    auto directory = std::filesystem::path{};
    auto *tmpf = test_platform::open_temporary_file(directory);
    EXPECT_NE(tmpf, nullptr);
    if (tmpf == nullptr) {
        test_platform::close_temporary_file(tmpf, directory);
        return {};
    }
    const auto stderr_fd = test_platform::file_descriptor(stderr);
    const auto orig_fd = test_platform::duplicate_descriptor(stderr_fd);
    test_platform::replace_descriptor(test_platform::file_descriptor(tmpf), stderr_fd);
    fn();
    std::fflush(stderr);
    test_platform::replace_descriptor(orig_fd, stderr_fd);
    detail::platform::close_file_descriptor(orig_fd);

    // Read from the temp file
    std::fseek(tmpf, 0, SEEK_END);
    auto sz = std::ftell(tmpf);
    std::fseek(tmpf, 0, SEEK_SET);
    auto result = std::string(static_cast<size_t>(sz), '\0');
    const auto bytes_read = std::fread(result.data(), 1, result.size(), tmpf);
    EXPECT_EQ(bytes_read, result.size());
    test_platform::close_temporary_file(tmpf, directory);
    return result;
}

} // namespace

// ─── log() delivers records to the receiver ─────────────────────────────────

TEST(LoggingTest, LogDeliversFormattedMessageToReceiver) {
    auto records = std::vector<CapturedRecord>{};
    detail::log<LogLevel::LL_INFO>(capture_records(records), "a={} b={} c={}", 1, "two", 3.0);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, LogLevel::LL_INFO);
    EXPECT_EQ(records[0].message, "a=1 b=two c=3");
}

TEST(LoggingTest, LogRecordCarriesCallerLocation) {
    auto records = std::vector<CapturedRecord>{};
    const auto expected_line = std::source_location::current().line() + 1;
    detail::log<LogLevel::LL_INFO>(capture_records(records), "located");
    ASSERT_EQ(records.size(), 1u);
    EXPECT_NE(std::string_view{records[0].location.file_name()}.find("logging_test.cpp"), std::string_view::npos);
    EXPECT_EQ(records[0].location.line(), expected_line);
}

TEST(LoggingTest, PrivateLogRecordOmitsCallerLocation) {
    auto records = std::vector<CapturedRecord>{};
    detail::log<LogLevel::LL_INFO, PrivateLogConfig>(capture_records(records), "private message");
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(std::string_view{records[0].location.file_name()}.find("logging_test.cpp"), std::string_view::npos);
}

TEST(LoggingTest, LogWithoutMessageDeliversEmptyRecord) {
    auto records = std::vector<CapturedRecord>{};
    detail::log<LogLevel::LL_WARNING>(capture_records(records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, LogLevel::LL_WARNING);
    EXPECT_TRUE(records[0].message.empty());
}

TEST(LoggingTest, LogNeverThrowsOrTerminates) {
    static_assert(noexcept(detail::log<LogLevel::LL_ERROR>(null_log_receiver(), "error {}", 1)));
    static_assert(noexcept(detail::log<LogLevel::LL_FATAL>(null_log_receiver())));
    auto records = std::vector<CapturedRecord>{};
    detail::log<LogLevel::LL_ERROR>(capture_records(records), "error");
    detail::log<LogLevel::LL_FATAL>(capture_records(records), "fatal");
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].level, LogLevel::LL_ERROR);
    EXPECT_EQ(records[1].level, LogLevel::LL_FATAL);
}

TEST(LoggingTest, NullReceiverSkipsLazyArguments) {
    auto calls = 0;
    detail::log<LogLevel::LL_ERROR>(null_log_receiver(), "{}", [&]() noexcept { return ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(LoggingTest, LogDebugSkipsLazyArgumentsInRelease) {
    auto records = std::vector<CapturedRecord>{};
    auto calls = 0;
    detail::log<LogLevel::LL_DEBUG>(capture_records(records), "lazy debug: {}", [&]() noexcept { return ++calls; });
    if constexpr (compile_config::is_debug_build) {
        EXPECT_EQ(calls, 1);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].message, "lazy debug: 1");
    } else {
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(records.empty());
    }
}

TEST(LoggingTest, LogEvaluatesLazyArgumentsOnce) {
    auto records = std::vector<CapturedRecord>{};
    auto calls = 0;
    detail::log<LogLevel::LL_INFO>(capture_records(records), "lazy info: {}", [&]() noexcept { return ++calls; });
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "lazy info: 1");
}

TEST(LoggingTest, LogAcceptsMoveOnlyLazyProducer) {
    auto records = std::vector<CapturedRecord>{};
    auto producer = [value = std::make_unique<int>(42)]() noexcept {
        return *value;
    };
    detail::log<LogLevel::LL_INFO>(capture_records(records), "move-only producer: {}", std::move(producer));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "move-only producer: 42");
}

TEST(LoggingTest, LogFormatsLazyLvalueReference) {
    auto records = std::vector<CapturedRecord>{};
    const auto value = std::string{"referenced value"};
    detail::log<LogLevel::LL_INFO>(capture_records(records), "{}",
                                   [&]() noexcept -> const std::string & { return value; });
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, value);
}

TEST(LoggingTest, LogFormatsLazyRvalueReference) {
    auto records = std::vector<CapturedRecord>{};
    auto value = std::string{"rvalue reference"};
    detail::log<LogLevel::LL_INFO>(capture_records(records), "{}",
                                   [&]() noexcept -> std::string && { return std::move(value); });
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "rvalue reference");
}

TEST(LoggingTest, NestedLogFromLazyArgumentIsDeliveredBeforeOuterRecord) {
    auto records = std::vector<CapturedRecord>{};
    const auto receiver = capture_records(records);
    detail::log<LogLevel::LL_WARNING>(receiver, "outer {}", [&]() noexcept {
        detail::log<LogLevel::LL_WARNING>(receiver, "inner");
        return 1;
    });
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].message, "inner");
    EXPECT_EQ(records[1].message, "outer 1");
}

TEST(LoggingTest, ConfiguredThresholdSkipsLowerLevelsIncludingError) {
    auto records = std::vector<CapturedRecord>{};
    const auto receiver = capture_records(records);
    auto calls = 0;
    const auto producer = [&]() noexcept {
        return ++calls;
    };
    detail::log<LogLevel::LL_DEBUG, HighThresholdLogConfig>(receiver, "{}", producer);
    detail::log<LogLevel::LL_INFO, HighThresholdLogConfig>(receiver, "{}", producer);
    detail::log<LogLevel::LL_WARNING, HighThresholdLogConfig>(receiver, "{}", producer);
    detail::log<LogLevel::LL_ERROR, HighThresholdLogConfig>(receiver, "{}", producer);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(records.empty());
}

TEST(LoggingTest, ConfiguredThresholdDeliversRecordsAtItsLevel) {
    auto records = std::vector<CapturedRecord>{};
    detail::log<LogLevel::LL_FATAL, HighThresholdLogConfig>(capture_records(records), "fatal remains enabled");
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "fatal remains enabled");
}

TEST(LoggingTest, LogTruncatesMessageAtConfiguredBufferCapacity) {
    auto records = std::vector<CapturedRecord>{};
    const auto message = std::string(256, 'x');
    detail::log<LogLevel::LL_INFO, SmallBufferLogConfig>(capture_records(records), "{}", message);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, std::string(SmallBufferLogConfig::buffer_size, 'x'));
}

TEST(LoggingTest, LargerConfiguredBufferPreservesLongMessages) {
    auto records = std::vector<CapturedRecord>{};
    const auto message = std::string(600, 'x');
    detail::log<LogLevel::LL_INFO, LargeBufferLogConfig>(capture_records(records), "{}", message);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, message);
}

// ─── stderr receiver ────────────────────────────────────────────────────────

TEST(LoggingTest, StderrReceiverWritesLevelAndMessage) {
    const auto output =
        capture_stderr([] { detail::log<LogLevel::LL_WARNING>(stderr_log_receiver(), "warn: {}", 99); });
    EXPECT_NE(output.find("WARN"), std::string::npos);
    EXPECT_NE(output.find("warn: 99"), std::string::npos);
}

TEST(LoggingTest, StderrReceiverWritesSourceLocation) {
    const auto output = capture_stderr([] { detail::log<LogLevel::LL_INFO>(stderr_log_receiver(), "loc check"); });
    EXPECT_NE(output.find("logging_test.cpp"), std::string::npos);
}

TEST(LoggingTest, StderrReceiverWritesProcessAndThreadIds) {
    const auto output = capture_stderr([] { detail::log<LogLevel::LL_INFO>(stderr_log_receiver(), "id check"); });
    const auto pid_str = std::string{"pid:"} + std::to_string(detail::platform::process_id());
    EXPECT_NE(output.find(pid_str), std::string::npos);
    EXPECT_NE(output.find("tid:"), std::string::npos);
}

TEST(LoggingTest, StderrReceiverLineEndsWithNewline) {
    const auto output = capture_stderr([] { detail::log<LogLevel::LL_INFO>(stderr_log_receiver(), "newline test"); });
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(output.back(), '\n');
}

TEST(LoggingTest, PrivateStderrReceiverOmitsSourceLocation) {
    const auto output = capture_stderr([] {
        detail::log<LogLevel::LL_INFO, PrivateLogConfig>(stderr_log_receiver<PrivateLogConfig>(), "private message");
    });
    EXPECT_NE(output.find("private message"), std::string::npos);
    EXPECT_EQ(output.find("logging_test.cpp"), std::string::npos);
    EXPECT_EQ(output.find("\033[35m"), std::string::npos);
    EXPECT_EQ(output.find(":0 "), std::string::npos);
}

TEST(LoggingTest, StderrReceiverTruncatesLineAtConfiguredBufferCapacity) {
    const auto message = std::string(256, 'x');
    const auto output = capture_stderr([&] {
        detail::log<LogLevel::LL_INFO, SmallBufferLogConfig>(stderr_log_receiver<SmallBufferLogConfig>(), "{}",
                                                             message);
    });
    ASSERT_EQ(output.size(), SmallBufferLogConfig::buffer_size);
    EXPECT_EQ(output.back(), '\n');
}

TEST(LoggingTest, StderrReceiverTruncatesPrefixToOneTerminatingNewline) {
    const auto output = capture_stderr([] {
        detail::log<LogLevel::LL_INFO, OneByteBufferLogConfig>(stderr_log_receiver<OneByteBufferLogConfig>(),
                                                               "message");
    });
    EXPECT_EQ(output, "\n");
}

TEST(LoggingTest, ExplicitDebugThresholdEnablesDebugInBothBuildModes) {
    const auto output = capture_stderr([] {
        detail::log<LogLevel::LL_DEBUG, SmallBufferLogConfig>(stderr_log_receiver<SmallBufferLogConfig>(),
                                                              "explicit debug");
    });
    EXPECT_NE(output.find("explicit debug"), std::string::npos);
}

TEST(LoggingTest, HmmModelLoadWritesNothingToStderr) {
    const auto output = capture_stderr(
        [] { [[maybe_unused]] const auto model = detail::HMModel{std::string_view{DICT_DIR "/hmm_model.utf8"}}; });
    EXPECT_TRUE(output.empty());
}

// ─── check() passes when expression is true ─────────────────────────────────

TEST(LoggingTest, CheckTrueDoesNotThrow) {
    EXPECT_NO_THROW(check(true, "should not fire"));
}

TEST(LoggingTest, CheckTrueWithArgsDoesNotThrow) {
    EXPECT_NO_THROW(check(true, "value={}", 42));
}

// ─── check() fails when expression is false ─────────────────────────────────

TEST(LoggingTest, CheckFalseThrowsRuntimeError) {
    EXPECT_THROW(check(false, "assertion failed: {}", "bad state"), std::runtime_error);
}

TEST(LoggingTest, CheckFalseExceptionContainsMessage) {
    try {
        check(false, "check detail: {}", 456);
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string_view{e.what()}.find("check detail: 456"), std::string_view::npos);
    }
}

TEST(LoggingTest, CheckExceptionPrefixesCallerLocation) {
    const auto expected_line = std::source_location::current().line() + 2;
    try {
        check(false, "loc in check");
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error &e) {
        EXPECT_TRUE(
            std::string_view{e.what()}.ends_with(std::format("logging_test.cpp:{}: loc in check", expected_line)));
    }
}

TEST(LoggingTest, CheckExceptionHasNoLogDecoration) {
    try {
        check(false, "undecorated");
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error &e) {
        const auto what = std::string_view{e.what()};
        EXPECT_EQ(what.find("<ERROR>"), std::string_view::npos);
        EXPECT_EQ(what.find("pid:"), std::string_view::npos);
        EXPECT_EQ(what.find("\033["), std::string_view::npos);
        EXPECT_FALSE(what.ends_with('\n'));
    }
}

TEST(LoggingTest, CheckDoesNotWriteToStderr) {
    const auto output = capture_stderr([] { EXPECT_THROW(check(false, "silent failure"), std::runtime_error); });
    EXPECT_TRUE(output.empty());
}

TEST(LoggingTest, CheckMessageIsNotLimitedByBufferCapacity) {
    const auto message = std::string(256, 'x');
    try {
        check<SmallBufferLogConfig>(false, "{}", message);
        FAIL() << "Expected ProjectError";
    } catch (const ProjectError &e) {
        EXPECT_EQ(std::string_view{e.what()}, message);
    }
}

TEST(LoggingTest, FailedCheckThrowsRegardlessOfConfiguredThreshold) {
    EXPECT_THROW(check<HighThresholdLogConfig>(false, "check remains enabled"), ProjectError);
}

TEST(LoggingTest, CheckUsesConfiguredException) {
    EXPECT_THROW(check<ProjectLogConfig>(false, "project check: {}", 42), ProjectError);
}

TEST(LoggingTest, PrivateCheckReportsOnlyTheMessage) {
    try {
        check<PrivateLogConfig>(false, "private check: {}", 42);
        FAIL() << "Expected ProjectError";
    } catch (const ProjectError &e) {
        EXPECT_EQ(std::string_view{e.what()}, "private check: 42");
    }
}

// ─── check() with no format string or args ───────────────────────────────────

TEST(LoggingTest, CheckNoArgsTrueDoesNotThrow) {
    EXPECT_NO_THROW(check(true));
}

TEST(LoggingTest, CheckNoArgsFalseThrows) {
    EXPECT_THROW(check(false), std::runtime_error);
}

TEST(LoggingTest, CheckWithoutMessageReportsCallerLocation) {
    const auto expected_line = std::source_location::current().line() + 2;
    try {
        check(false);
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error &e) {
        EXPECT_TRUE(std::string_view{e.what()}.ends_with(std::format("logging_test.cpp:{}", expected_line)));
    }
}

TEST(LoggingTest, CheckWithoutMessageUsesConfiguredException) {
    EXPECT_THROW(check<ProjectLogConfig>(false), ProjectError);
}

TEST(LoggingTest, PrivateCheckWithoutMessageHasEmptyDiagnostic) {
    try {
        check<PrivateLogConfig>(false);
        FAIL() << "Expected ProjectError";
    } catch (const ProjectError &e) {
        EXPECT_EQ(std::string_view{e.what()}, "");
    }
}

// ─── check() lazy evaluation ────────────────────────────────────────────────

TEST(LoggingTest, CheckTrueSkipsLazyArguments) {
    auto calls = 0;
    EXPECT_NO_THROW(check(true, "unused: {}", [&]() noexcept { return ++calls; }));
    EXPECT_EQ(calls, 0);
}

TEST(LoggingTest, CheckEvaluatesPredicateOnce) {
    auto calls = 0;
    EXPECT_NO_THROW(check([&] { return ++calls == 1; }, "predicate failed"));
    EXPECT_EQ(calls, 1);
}

TEST(LoggingTest, CheckWithoutMessageEvaluatesPredicateOnce) {
    auto calls = 0;
    EXPECT_THROW(check([&] { return ++calls == 0; }), std::runtime_error);
    EXPECT_EQ(calls, 1);
}

TEST(LoggingTest, CheckFalseEvaluatesLazyArgumentsOnce) {
    auto calls = 0;
    try {
        check(false, "lazy check: {}", [&]() noexcept { return ++calls; });
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string_view{e.what()}.find("lazy check: 1"), std::string_view::npos);
    }
    EXPECT_EQ(calls, 1);
}

TEST(LoggingTest, CheckPropagatesPredicateExceptions) {
    const auto predicate = []() -> bool {
        throw std::runtime_error{"input validation failed"};
    };
    EXPECT_THROW(check(predicate), std::runtime_error);
    EXPECT_THROW(check(predicate, "external input"), std::runtime_error);
}

// ─── LogFormatString captures source_location ───────────────────────────────

TEST(LoggingTest, LogFormatStringCapturesLocation) {
    auto fmt = detail::LogFormatString<LogConfig, int>{"val={}", std::source_location::current()};
    EXPECT_NE(std::string_view{fmt.loc.file_name()}.find("logging_test.cpp"), std::string_view::npos);
    EXPECT_GT(fmt.loc.line(), 0u);
}

// ─── assert_check() ─────────────────────────────────────────────────────────

TEST(LoggingTest, AssertionAndInputChecksHaveDistinctExceptionContracts) {
    static_assert(noexcept(assert_check([] { return true; })));
    static_assert(noexcept(assert_check([] { return true; }, "invariant {}", 1)));
    static_assert(!noexcept(check(true)));
    static_assert(!noexcept(check(true, "input {}", 1)));
}

TEST(LoggingTest, AssertCheckTerminatesWhenPredicateThrows) {
    const auto predicate = []() -> bool {
        throw std::runtime_error{"broken invariant predicate"};
    };
    // Keep GoogleTest's generated labels in an ordinary branch for MSVC.
    if (compile_config::is_debug_build) {
        EXPECT_EXIT(
            {
                prepare_assertion_death_test();
                assert_check(predicate, "internal invariant");
            },
            ::testing::ExitedWithCode(73), "");
    } else {
        EXPECT_NO_THROW(assert_check(predicate, "internal invariant"));
    }
}

TEST(LoggingTest, AssertCheckWithoutMessageTerminatesWhenPredicateThrows) {
    const auto predicate = []() -> bool {
        throw std::runtime_error{"broken invariant predicate"};
    };
    if (compile_config::is_debug_build) {
        EXPECT_EXIT(
            {
                prepare_assertion_death_test();
                assert_check(predicate);
            },
            ::testing::ExitedWithCode(73), "");
    } else {
        EXPECT_NO_THROW(assert_check(predicate));
    }
}

TEST(LoggingTest, AssertCheckTrueDoesNotThrow) {
    EXPECT_NO_THROW(assert_check([] { return true; }, "should not fire"));
}

TEST(LoggingTest, AssertCheckTrueWithArgsDoesNotThrow) {
    EXPECT_NO_THROW(assert_check([] { return true; }, "value={}", 42));
}

TEST(LoggingTest, AssertCheckFalseTerminatesInDebug) {
#ifndef NDEBUG
    EXPECT_EXIT(
        {
            prepare_assertion_death_test();
            assert_check([] { return false; }, "assert failed: {}", "bad");
        },
        ::testing::ExitedWithCode(73), "assert failed: bad");
#endif
}

TEST(LoggingTest, AssertCheckFalseDiagnosticContainsMessage) {
#ifndef NDEBUG
    EXPECT_EXIT(
        {
            prepare_assertion_death_test();
            assert_check([] { return false; }, "assert detail: {}", 789);
        },
        ::testing::ExitedWithCode(73), "FATAL.*assert detail: 789");
#endif
}

TEST(LoggingTest, AssertCheckFalseDiagnosticContainsSourceLocation) {
#ifndef NDEBUG
    EXPECT_EXIT(
        {
            prepare_assertion_death_test();
            assert_check([] { return false; }, "loc in assert_check");
        },
        ::testing::ExitedWithCode(73), "logging_test.cpp");
#endif
}

// ─── assert_check() with no format string or args ────────────────────────────

TEST(LoggingTest, AssertCheckNoArgsTrueDoesNotThrow) {
    EXPECT_NO_THROW(assert_check([] { return true; }));
}

TEST(LoggingTest, AssertCheckNoArgsFalseTerminatesInDebug) {
#ifndef NDEBUG
    EXPECT_EXIT(
        {
            prepare_assertion_death_test();
            assert_check([] { return false; });
        },
        ::testing::ExitedWithCode(73), "FATAL");
#endif
}

TEST(LoggingTest, AssertCheckNoArgsFalseDiagnosticContainsLocation) {
#ifndef NDEBUG
    EXPECT_EXIT(
        {
            prepare_assertion_death_test();
            assert_check([] { return false; });
        },
        ::testing::ExitedWithCode(73), "FATAL.*logging_test.cpp");
#endif
}

TEST(LoggingTest, AssertCheckPredicateNotCalledInRelease) {
    auto called = false;
    auto predicate = [&called] {
        called = true;
        return true;
    };
    assert_check(predicate, "should not matter");
#ifdef NDEBUG
    EXPECT_FALSE(called);
#else
    EXPECT_TRUE(called);
#endif
}

TEST(LoggingTest, AssertCheckTrueSkipsLazyArguments) {
    auto calls = 0;
    EXPECT_NO_THROW(assert_check([] { return true; }, "unused: {}", [&]() noexcept { return ++calls; }));
    EXPECT_EQ(calls, 0);
}

TEST(LoggingTest, AssertCheckSkipsPredicateAndLazyArgumentsInRelease) {
    auto predicate_calls = 0;
    auto argument_calls = 0;
    const auto predicate = [&] {
        return ++predicate_calls == 0;
    };
    const auto argument = [&]() noexcept {
        return ++argument_calls;
    };
    if (compile_config::is_debug_build) {
        EXPECT_EXIT(
            {
                prepare_assertion_death_test();
                assert_check<PrivateLogConfig>(predicate, "lazy assert: {}, predicate calls: {}", argument,
                                               [&]() noexcept { return predicate_calls; });
            },
            ::testing::ExitedWithCode(73), "lazy assert: 1, predicate calls: 1");
    } else {
        EXPECT_NO_THROW(assert_check<PrivateLogConfig>(predicate, "lazy assert: {}", argument));
        EXPECT_EQ(predicate_calls, 0);
        EXPECT_EQ(argument_calls, 0);
    }
}

TEST(LoggingTest, AssertCheckTerminatesWithConfiguredThreshold) {
    if (compile_config::is_debug_build) {
        EXPECT_EXIT(
            {
                prepare_assertion_death_test();
                assert_check<HighThresholdLogConfig>([] { return false; }, "assertion remains enabled");
            },
            ::testing::ExitedWithCode(73), "FATAL.*assertion remains enabled");
    }
}

TEST(LoggingTest, AssertCheckNoArgsSkipsPredicateInRelease) {
    auto calls = 0;
    assert_check([&] { return ++calls == 1; });
    EXPECT_EQ(calls, compile_config::is_debug_build ? 1 : 0);
}

TEST(LoggingTest, AssertCheckSupportsConstantEvaluation) {
    constexpr auto valid = [] {
        assert_check([] { return true; }, "constant assertion: {}", 42);
        assert_check([] { return true; });
        return true;
    }();
    static_assert(valid);
    EXPECT_TRUE(valid);
}

// ─── Compile-time contracts ─────────────────────────────────────────────────

TEST(LoggingTest, LogExceptionRejectsInvalidExceptionTypes) {
    static_assert(LogException<ProjectError>);
    static_assert(LogConfiguration<ProjectLogConfig>);
    static_assert(!LogException<std::string>);
    static_assert(!LogException<std::exception>);
}

TEST(LoggingTest, LoggingApisRejectInvalidConfigurations) {
    constexpr auto rejected = []<typename Config>() {
        return !requires { stderr_log_receiver<Config>(); }
               && !requires { detail::log<LogLevel::LL_ERROR, Config>(null_log_receiver()); }
               && !requires { detail::log<LogLevel::LL_ERROR, Config>(null_log_receiver(), "invalid config"); }
               && !requires { check<Config>(true); } && !requires { check<Config>(true, "invalid config"); }
               && !requires { assert_check<Config>([] { return true; }); }
               && !requires { assert_check<Config>([] { return true; }, "invalid config"); };
    };
    static_assert(rejected.template operator()<MissingExceptionConfig>());
    static_assert(rejected.template operator()<MissingSourceLocationConfig>());
    static_assert(rejected.template operator()<InvalidExceptionConfig>());
    static_assert(rejected.template operator()<IntegerSourceLocationConfig>());
    static_assert(rejected.template operator()<NonConstantSourceLocationConfig>());
    static_assert(rejected.template operator()<InvalidLogLevelConfig>());
    static_assert(rejected.template operator()<NonConstantLogLevelConfig>());
    static_assert(rejected.template operator()<EmptyBufferLogConfig>());
    static_assert(rejected.template operator()<NonConstantBufferLogConfig>());
}

TEST(LoggingTest, DiagnosticApisRequireNothrowLazyArguments) {
    constexpr auto accepted = []<typename Argument>() {
        return requires(Argument &&argument) {
            detail::log<LogLevel::LL_INFO>(null_log_receiver(), "{}", std::forward<Argument>(argument));
        } &&
            requires(Argument && argument)
        {
            check(true, "{}", std::forward<Argument>(argument));
        }
        &&
            requires(Argument && argument)
        {
            assert_check([] { return true; }, "{}", std::forward<Argument>(argument));
        };
    };
    constexpr auto rejected = []<typename Argument>() {
        return !requires(Argument &&argument) {
            detail::log<LogLevel::LL_INFO>(null_log_receiver(), "{}", std::forward<Argument>(argument));
        } && !requires(Argument &&argument) { check(true, "{}", std::forward<Argument>(argument)); }
               && !requires(Argument &&argument) {
                      assert_check([] { return true; }, "{}", std::forward<Argument>(argument));
                  };
    };
    static_assert(accepted.template operator()<decltype([]() noexcept { return 1; })>());
    static_assert(rejected.template operator()<decltype([] { return 1; })>());
}

TEST(LoggingTest, AssertCheckRejectsInvalidPredicates) {
    constexpr auto rejected = []<typename Predicate>() {
        return !requires(Predicate &&predicate) { assert_check(std::forward<Predicate>(predicate)); }
               && !requires(Predicate &&predicate) {
                      assert_check(std::forward<Predicate>(predicate), "invalid predicate");
                  };
    };
    static_assert(rejected.template operator()<bool>());
    static_assert(rejected.template operator()<decltype([] {})>());
    static_assert(rejected.template operator()<decltype([](int) { return true; })>());
    static_assert(rejected.template operator()<decltype([] { return std::string{}; })>());
}
