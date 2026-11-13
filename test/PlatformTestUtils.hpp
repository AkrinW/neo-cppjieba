#pragma once

#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/util/FileIO.hpp"

#include "TestUtils.hpp"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>

#include <sys/resource.h>
#include <sys/stat.h>
#endif

namespace test_platform {

// Binary stream modes used by the file and logging fixtures.
enum class FileMode { Read, Write, ReadWrite };

// Open a native filesystem path; the test retains responsibility for closing the stream.
inline
auto open_binary_file(const std::filesystem::path &path, FileMode mode) -> std::FILE * {
    const auto index = static_cast<size_t>(mode);
#ifdef _WIN32
    constexpr auto modes = std::array{L"rb", L"wb", L"w+b"};
    assert(index < modes.size());
    return ::_wfopen(path.c_str(), modes[index]);
#else
    constexpr auto modes = std::array{"rb", "wb", "w+b"};
    assert(index < modes.size());
    return std::fopen(path.c_str(), modes[index]);
#endif
}

// Obtain the descriptor shared by stdio and the platform's unbuffered I/O API.
inline
auto file_descriptor(std::FILE *file) noexcept -> int {
    assert(file != nullptr);
#ifdef _WIN32
    return ::_fileno(file);
#else
    return ::fileno(file);
#endif
}

// Preserve a descriptor while stderr is redirected by a logging test.
inline
auto duplicate_descriptor(int fd) noexcept -> int {
#ifdef _WIN32
    return ::_dup(fd);
#else
    return ::dup(fd);
#endif
}

// Replace a descriptor, normalizing the different successful dup2 return values.
inline
auto replace_descriptor(int source, int destination) noexcept -> bool {
#ifdef _WIN32
    return ::_dup2(source, destination) != -1;
#else
    return ::dup2(source, destination) != -1;
#endif
}

// Windows tmpfile can require root-directory access; remember its explicit temporary directory instead.
inline
auto open_temporary_file(std::filesystem::path &directory) -> std::FILE * {
#ifdef _WIN32
    directory = create_temp_directory("neo-log-capture-");
    return open_binary_file(directory / "stderr", FileMode::ReadWrite);
#else
    directory.clear();
    return std::tmpfile();
#endif
}

// Remove temporary capture storage after its stream has been closed, including failed opens.
inline
auto close_temporary_file(std::FILE *file, const std::filesystem::path &directory) -> void {
    if (file != nullptr) {
        std::fclose(file);
    }
    if (!directory.empty()) {
        std::filesystem::remove_all(directory);
    }
}

// Match the native error domain used when reading a write-only descriptor.
inline
auto write_only_read_error() noexcept -> std::error_code {
#ifdef _WIN32
    return {ERROR_ACCESS_DENIED, std::system_category()};
#else
    return {EBADF, std::generic_category()};
#endif
}

// The native code a Win32 API or POSIX call reports for a missing file.
inline
auto missing_file_system_error() noexcept -> int {
#ifdef _WIN32
    return static_cast<int>(ERROR_FILE_NOT_FOUND);
#else
    return ENOENT;
#endif
}

// Disable POSIX core dumps inside the subprocess that tests external file truncation.
inline
auto prepare_fileio_death_test() noexcept -> void {
#ifndef _WIN32
    const auto core_limit = ::rlimit{};
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
        std::_Exit(2);
    }
#endif
}

// POSIX readers keep the file they opened after a rename replaces its name; Windows refuses to replace a file
// that a reader opened without FILE_SHARE_DELETE.
inline
auto replacement_keeps_open_readers() noexcept -> bool {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

// UTF-8 validation is specific to Windows path conversion; POSIX accepts arbitrary path bytes.
inline
auto expect_invalid_utf8_path_rejected() -> void {
#ifdef _WIN32
    EXPECT_THROW(neo_cppjieba::detail::read_file(std::string_view{"\xff"}), neo_cppjieba::LogConfig::Exception);
#else
    GTEST_SKIP() << "POSIX paths do not require UTF-8";
#endif
}

// POSIX permits opening directories, which exercises constructor cleanup after a successful open.
inline
auto expect_rejected_directory_closes_descriptor(const std::filesystem::path &directory) -> void {
#ifdef _WIN32
    (void)directory;
    GTEST_SKIP() << "Windows rejects directories during open";
#else
    const auto path = directory.string();
    const auto descriptor_before = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_NE(descriptor_before, -1);
    ASSERT_EQ(::close(descriptor_before), 0);

    EXPECT_THROW(neo_cppjieba::detail::read_file(path), neo_cppjieba::LogConfig::Exception);

    // open reuses the lowest available descriptor; a failed constructor must leave it available.
    const auto descriptor_after = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_NE(descriptor_after, -1);
    EXPECT_EQ(descriptor_after, descriptor_before);
    EXPECT_EQ(::close(descriptor_after), 0);
#endif
}

// Exercise nonblocking FIFO rejection only on platforms that provide POSIX FIFOs.
inline
auto expect_fifo_rejected_without_waiting(const std::filesystem::path &fifo) -> void {
#ifdef _WIN32
    (void)fifo;
    GTEST_SKIP() << "Requires POSIX FIFOs";
#else
    const auto path = fifo.string();
    ASSERT_EQ(::mkfifo(path.c_str(), S_IRUSR | S_IWUSR), 0);

    // Bound a regression to the subprocess so a blocking open cannot hang the test runner.
    EXPECT_EXIT(
        {
            ::alarm(5);
            try {
                const auto file = neo_cppjieba::detail::read_file(path);
            } catch (const neo_cppjieba::LogConfig::Exception &error) {
                // Failed checks no longer log, so report the diagnostic for the death-test matcher.
                std::fputs(error.what(), stderr);
                std::_Exit(0);
            }
            std::_Exit(1);
        },
        ::testing::ExitedWithCode(0), "not a regular file");
#endif
}

} // namespace test_platform
