#pragma once

#include "neo/Config.hpp"
#include "neo/util/Platform.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <functional>
#include <source_location>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace neo_cppjieba {

namespace detail {
// The library has no log call sites; receiver-based logging is archived here for future diagnostics.
// Only check and assert_check below are public.

// Borrowed view of one diagnostic; message and location are valid only while the receiver callback runs.
struct LogRecordView {
    LogLevel level;
    std::string_view message;
    std::source_location location;
};

// Non-owning receiver passed to each log call and invoked synchronously on the logging thread.
// A null receive function discards records before message formatting or lazy argument evaluation.
struct LogReceiver {
    void *context;
    void (*receive)(void *context, const LogRecordView &record) noexcept;
};

// LogLevel enum to string mapping for log output.
inline constexpr auto LOG_LEVEL_ARRAY = std::array<std::string_view, 5>{"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
constexpr auto log_level_name(LogLevel level) noexcept -> std::string_view {
    // Logging cannot diagnose its own invalid level through assert_check without recursion.
    assert(static_cast<size_t>(level) < LOG_LEVEL_ARRAY.size());
    return LOG_LEVEL_ARRAY[static_cast<size_t>(level)];
}

// ANSI color escape sequences per log level.
inline constexpr auto LOG_LEVEL_COLOR_ARRAY = std::array<std::string_view, 5>{
    "\033[36m",   // DEBUG: cyan
    "\033[32m",   // INFO: green
    "\033[33m",   // WARNING: yellow
    "\033[31m",   // ERROR: red
    "\033[1;31m", // FATAL: bold red
};
inline constexpr auto LOG_COLOR_RESET = std::string_view{"\033[0m"};

constexpr auto log_level_color(LogLevel level) noexcept -> std::string_view {
    assert(static_cast<size_t>(level) < LOG_LEVEL_COLOR_ARRAY.size());
    return LOG_LEVEL_COLOR_ARRAY[static_cast<size_t>(level)];
}

inline auto stderr_is_tty() noexcept -> bool {
    static const auto result = platform::stderr_is_tty();
    return result;
}

// kCompileTimeMinLevel is a compile-time constant that indicates the minimum log level to be compiled into the binary.
inline constexpr auto kCompileTimeMinLevel = LogConfig::min_level;
inline constexpr auto kNoDebug = !compile_config::is_debug_build;

template <typename Condition>
concept CheckPredicate = std::invocable<Condition> && std::convertible_to<std::invoke_result_t<Condition>, bool>;

template <typename Condition>
concept CheckCondition = std::convertible_to<Condition, bool> || CheckPredicate<Condition>;

// Treat zero-argument callables as lazy values while preserving ordinary string arguments.
template <typename Value>
concept LogArgInvocable = std::invocable<Value> && !std::is_convertible_v<Value, std::string_view>
                          && !std::is_convertible_v<Value, const char *>;

// Lazy producers must not throw, so formatting a diagnostic never replaces the caller's control flow.
template <typename Value>
concept NothrowLogArg = !LogArgInvocable<Value> || std::is_nothrow_invocable_v<Value>;

// Invoke diagnostic producers only after a log level or failed check has selected the logging path.
template <typename Value>
[[nodiscard]] auto eval_log_arg(Value &&arg) -> decltype(auto) {
    if constexpr (LogArgInvocable<Value>) {
        return std::invoke(std::forward<Value>(arg));
    } else {
        return std::forward<Value>(arg);
    }
}

template <typename Value>
using LogRawEvalType = decltype(eval_log_arg(std::declval<Value>()));

// Match std::format's deduction: retain lvalue references and normalize rvalue references to values.
template <typename Value>
using LogEvalType = std::conditional_t<std::is_lvalue_reference_v<LogRawEvalType<Value>>, LogRawEvalType<Value>,
                                       std::remove_reference_t<LogRawEvalType<Value>>>;

// Evaluate a bool-like value or predicate exactly once for a check.
template <CheckCondition Condition>
[[nodiscard]] constexpr auto eval_check_condition(Condition &&condition) -> bool {
    if constexpr (CheckPredicate<Condition>) {
        return static_cast<bool>(std::invoke(std::forward<Condition>(condition)));
    } else {
        return static_cast<bool>(std::forward<Condition>(condition));
    }
}

// LOG_TIME_FORMAT is the format string used to format the timestamp in log messages.
inline constexpr auto LOG_TIME_FORMAT = "%Y-%m-%d %H:%M:%S";

// LOG_MAX_TIME_BUFFER_SIZE is the maximum size of the buffer used to store the formatted timestamp in log messages.
inline constexpr auto LOG_MAX_TIME_BUFFER_SIZE = size_t{32};

// LOG_MAX_BUFFER_SIZE is the maximum size of the buffer used to store the formatted log message before writing to
// stderr.
inline constexpr auto LOG_MAX_BUFFER_SIZE = LogConfig::buffer_size;

// cur_time fills the provided buffer with the current time formatted according to LOG_TIME_FORMAT.
inline auto cur_time() noexcept -> std::array<char, LOG_MAX_TIME_BUFFER_SIZE> {
    const auto tmNow = platform::local_time(std::time(nullptr));
    auto buf = std::array<char, LOG_MAX_TIME_BUFFER_SIZE>{};
    std::strftime(buf.data(), buf.size(), LOG_TIME_FORMAT, &tmNow);
    return buf;
}

// Colored prefix format:
//   dim timestamp, dim [pid/tid], level-colored <LEVEL>, magenta file:line
inline constexpr auto LOG_COLOR_PREFIX_FMT =
    "\033[90m{}\033[0m[\033[90mpid:{} tid:{:04x}\033[0m]{}<{}>\033[0m\033[35m{}:{}\033[0m ";
inline constexpr auto LOG_PLAIN_PREFIX_FMT = "{}[pid:{} tid:{:04x}]<{}>{}:{} ";
inline constexpr auto LOG_COLOR_PRIVATE_PREFIX_FMT =
    "\033[90m{}\033[0m[\033[90mpid:{} tid:{:04x}\033[0m]{}<{}>\033[0m ";
inline constexpr auto LOG_PLAIN_PRIVATE_PREFIX_FMT = "{}[pid:{} tid:{:04x}]<{}> ";

// format_log_message formats a message into a caller-owned buffer and truncates it at the buffer capacity.
// The returned view borrows that buffer.
template <std::size_t Capacity, typename... Args>
inline auto format_log_message(std::array<char, Capacity> &buffer, std::format_string<LogEvalType<Args>...> fmt,
                               Args &&...args) -> std::string_view {
    const auto result = std::format_to_n(buffer.begin(), Capacity, fmt, eval_log_arg(std::forward<Args>(args))...);
    return std::string_view{buffer.data(), static_cast<size_t>(result.out - buffer.begin())};
}

// write_stderr_record writes one record to stderr as a single line. The prefix, message, and terminating newline are
// truncated to Config::buffer_size bytes. The receiver context is unused.
template <LogConfiguration Config>
inline auto write_stderr_record(void * /*context*/, const LogRecordView &record) noexcept -> void {
    constexpr auto buffer_size = Config::buffer_size;
    constexpr auto color_prefix_fmt =
        Config::show_source_location ? LOG_COLOR_PREFIX_FMT : LOG_COLOR_PRIVATE_PREFIX_FMT;
    constexpr auto plain_prefix_fmt =
        Config::show_source_location ? LOG_PLAIN_PREFIX_FMT : LOG_PLAIN_PRIVATE_PREFIX_FMT;
    auto buffer = std::array<char, buffer_size>{};
    const auto time_buf = cur_time();
    const auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto use_color = stderr_is_tty();
    const auto pid = platform::process_id();
    const auto tid_short = static_cast<uint16_t>(tid_hash);
    const auto &loc = record.location;

    size_t prefix_len = 0;
    if (use_color) {
        const auto it =
            std::format_to_n(buffer.begin(), buffer_size, color_prefix_fmt, time_buf.data(), pid, tid_short,
                             log_level_color(record.level), log_level_name(record.level), loc.file_name(), loc.line());
        prefix_len = static_cast<size_t>(it.out - buffer.begin());
    } else {
        const auto it = std::format_to_n(buffer.begin(), buffer_size, plain_prefix_fmt, time_buf.data(), pid, tid_short,
                                         log_level_name(record.level), loc.file_name(), loc.line());
        prefix_len = static_cast<size_t>(it.out - buffer.begin());
    }

    const auto message_len = std::min(record.message.size(), buffer_size - prefix_len);
    std::copy_n(record.message.data(), message_len, buffer.data() + prefix_len);
    auto total = prefix_len + message_len;

    if (total < buffer_size) {
        buffer[total] = '\n';
        total += 1;
    } else {
        buffer[buffer_size - 1] = '\n';
        total = buffer_size;
    }

    std::fwrite(buffer.data(), sizeof(char), total, stderr);
}

// Failed assertions bypass log receivers, write directly to stderr, and invoke the terminate handler.
template <LogConfiguration Config, typename... Args>
[[noreturn]] inline auto fail_assertion(std::format_string<LogEvalType<Args>...> fmt, const std::source_location &loc,
                                        Args &&...args) noexcept -> void {
    auto buffer = std::array<char, Config::buffer_size>{};
    const auto message = format_log_message(buffer, fmt, std::forward<Args>(args)...);
    write_stderr_record<Config>(nullptr, LogRecordView{LogLevel::LL_FATAL, message, loc});
    std::fflush(stderr);
    std::terminate();
}

// Build a plain version for the exception message (no ANSI codes).
// The caller location prefixes the message only when Config shows source locations.
template <LogConfiguration Config, typename... Args>
[[noreturn]] inline auto throw_check_failure(std::format_string<LogEvalType<Args>...> fmt,
                                             [[maybe_unused]] const std::source_location &loc, Args &&...args) -> void {
    auto message = std::format(fmt, eval_log_arg(std::forward<Args>(args))...);
    if constexpr (!Config::show_source_location) {
        throw typename Config::Exception{std::move(message)};
    } else if (message.empty()) {
        throw typename Config::Exception{std::format("{}:{}", loc.file_name(), loc.line())};
    } else {
        throw typename Config::Exception{std::format("{}:{}: {}", loc.file_name(), loc.line(), message)};
    }
}

// log_impl is the internal function that performs the actual logging. It formats the log message and delivers it to
// the receiver.
template <LogLevel Level, LogConfiguration Config, typename... Args>
inline auto log_impl(const LogReceiver &receiver, std::format_string<LogEvalType<Args>...> fmt,
                     const std::source_location &loc, Args &&...args) noexcept -> void {
    if constexpr (Level < Config::min_level) {
        return;
    }
    if (receiver.receive == nullptr) {
        return;
    }

    auto buffer = std::array<char, Config::buffer_size>{};
    const auto message = format_log_message(buffer, fmt, std::forward<Args>(args)...);
    receiver.receive(receiver.context, LogRecordView{Level, message, loc});
    // Diagnostics must not throw into the caller even if a formatter throws.
    // The noexcept specification enforces this on diagnostic failure.
}

// LogFormatString is a helper struct that holds a format string and its source location for logging purposes. It allows
// for compile-time format string checking and provides context for log messages.
template <LogConfiguration Config, typename... Args>
struct LogFormatString {
    std::format_string<Args...> fmt;
    std::source_location loc;

    template <typename StringType>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload): built from format literals; copies use the copy ctor.
    consteval LogFormatString(StringType &&s, const std::source_location &l = std::source_location::current()) noexcept
        : fmt(std::forward<StringType>(s)), loc(Config::show_source_location ? l : std::source_location{}) {
    }
};

// Receiver that writes each record to stderr with Config's line capacity and source location policy.
template <LogConfiguration Config = LogConfig>
[[nodiscard]] constexpr auto stderr_log_receiver() noexcept -> LogReceiver {
    return LogReceiver{nullptr, &write_stderr_record<Config>};
}

// Receiver that discards every record, so log calls skip message formatting and lazy arguments.
[[nodiscard]] constexpr auto null_log_receiver() noexcept -> LogReceiver {
    return LogReceiver{nullptr, nullptr};
}

// log is a helper function that formats a log message with the given format string and arguments, and logs it at the
// specified log level.
// Pass expensive arguments as zero-argument callables so filtered log levels skip their computation.
template <LogLevel Level, LogConfiguration Config = LogConfig, typename... Args>
    requires(NothrowLogArg<Args> && ...)
inline auto log(const LogReceiver &receiver, std::type_identity_t<LogFormatString<Config, LogEvalType<Args>...>> fmt,
                Args &&...args) noexcept -> void {
    log_impl<Level, Config>(receiver, fmt.fmt, fmt.loc, std::forward<Args>(args)...);
}

// Log an empty message with the same severity and configuration rules.
template <LogLevel Level, LogConfiguration Config = LogConfig>
inline auto log(const LogReceiver &receiver, const std::source_location &loc = std::source_location::current()) noexcept
    -> void {
    log_impl<Level, Config>(receiver, "", Config::show_source_location ? loc : std::source_location{});
}
} // namespace detail

// check is a helper function that evaluates an expression and throws Config::Exception if the expression is false.
// The exception carries the formatted message without log decoration; no log record is delivered.
// Failed checks throw regardless of Config::min_level.
template <LogConfiguration Config = LogConfig, detail::CheckCondition Condition, typename... Args>
    requires(detail::NothrowLogArg<Args> && ...)
inline auto check(Condition &&condition,
                  std::type_identity_t<detail::LogFormatString<Config, detail::LogEvalType<Args>...>> fmt,
                  Args &&...args) -> void {
    if (!detail::eval_check_condition(std::forward<Condition>(condition))) [[unlikely]] {
        detail::throw_check_failure<Config>(fmt.fmt, fmt.loc, std::forward<Args>(args)...);
    }
}

// Check a condition without an additional diagnostic message.
template <LogConfiguration Config = LogConfig, detail::CheckCondition Condition>
inline auto check(Condition &&condition, const std::source_location &loc = std::source_location::current()) -> void {
    if (!detail::eval_check_condition(std::forward<Condition>(condition))) [[unlikely]] {
        detail::throw_check_failure<Config>("", Config::show_source_location ? loc : std::source_location{});
    }
}

// assert_check evaluates a predicate lambda only in debug builds (NDEBUG not defined).
// In release builds the lambda is never invoked, avoiding side-effect evaluation.
// Failed assertions terminate; valid predicates can also be checked during constant evaluation.
template <LogConfiguration Config = LogConfig, detail::CheckPredicate F, typename... Args>
    requires(detail::NothrowLogArg<Args> && ...)
constexpr auto assert_check(F &&predicate,
                            std::type_identity_t<detail::LogFormatString<Config, detail::LogEvalType<Args>...>> fmt,
                            Args &&...args) noexcept -> void {
    if constexpr (detail::kNoDebug) {
        return;
    } else {
        if (!detail::eval_check_condition(std::forward<F>(predicate))) [[unlikely]] {
            detail::fail_assertion<Config>(fmt.fmt, fmt.loc, std::forward<Args>(args)...);
        }
    }
}

// Apply the same debug-only terminating assertion when no diagnostic message is supplied.
template <LogConfiguration Config = LogConfig, detail::CheckPredicate F>
constexpr auto assert_check(F &&predicate, const std::source_location &loc = std::source_location::current()) noexcept
    -> void {
    if constexpr (detail::kNoDebug) {
        return;
    } else {
        if (!detail::eval_check_condition(std::forward<F>(predicate))) [[unlikely]] {
            detail::fail_assertion<Config>("", Config::show_source_location ? loc : std::source_location{});
        }
    }
}

} // namespace neo_cppjieba
