#pragma once

#include <cassert>
#include <ctime>

#ifdef _WIN32
#include <cstdio>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace neo_cppjieba::detail::platform {

// Return the current process identifier without exposing the platform API to logging.
inline
auto process_id() noexcept -> int {
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}

// Query the terminal attached to stderr; the logging layer controls caching.
inline
auto stderr_is_tty() noexcept -> bool {
#ifdef _WIN32
    return ::_isatty(::_fileno(stderr)) != 0;
#else
    return ::isatty(STDERR_FILENO) != 0;
#endif
}

// Convert into caller-owned calendar fields using the platform's thread-safe API.
inline
auto local_time(std::time_t time) noexcept -> std::tm {
    auto result = std::tm{};
#ifdef _WIN32
    const auto error = ::localtime_s(&result, &time);
    assert(error == 0);
    (void)error;
#else
    const auto *converted = ::localtime_r(&time, &result);
    assert(converted != nullptr);
    (void)converted;
#endif
    return result;
}

} // namespace neo_cppjieba::detail::platform
