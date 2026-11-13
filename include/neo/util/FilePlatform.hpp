#pragma once

#include "neo/util/Logging.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include <sys/stat.h>

#ifdef _WIN32
// Keep the Windows SDK from defining min/max macros in public header consumers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#endif

namespace neo_cppjieba::detail::platform {

// Category of an operating-system error code, mirroring std::generic_category and std::system_category.
enum class ErrorCategory : uint8_t { Generic, System };

// An operating-system error code whose text is produced only while a diagnostic is formatted.
struct ErrorMessage {
    ErrorCategory category;
    int code;
};

// Capacity for one error description; longer platform text is truncated.
inline constexpr auto kErrorMessageCapacity = size_t{256};

#ifndef _WIN32
// GNU strerror_r returns the message, which may be a static string rather than the buffer.
inline
auto strerror_text(const char *message, std::span<const char> /*buffer*/) noexcept -> std::string_view {
    return message;
}

// XSI strerror_r fills the buffer and returns zero on success.
inline
auto strerror_text(int status, std::span<const char> buffer) noexcept -> std::string_view {
    return status == 0 ? std::string_view{buffer.data()} : std::string_view{"Unknown error"};
}
#endif

// Write the platform text for an error code into the buffer without allocating; the view borrows the buffer.
inline
auto error_message_text(const ErrorMessage &error, std::span<char, kErrorMessageCapacity> buffer) noexcept
    -> std::string_view {
#ifdef _WIN32
    if (error.category == ErrorCategory::System) {
        const auto length = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                             static_cast<DWORD>(error.code), 0, buffer.data(),
                                             static_cast<DWORD>(buffer.size()), nullptr);
        // FormatMessage ends system text with a line break, but diagnostics embed it in one line.
        const auto text = std::string_view{buffer.data(), length};
        const auto end = text.find_last_not_of(" \r\n");
        return end == std::string_view::npos ? std::string_view{"Unknown error"} : text.substr(0, end + 1);
    }
    return ::strerror_s(buffer.data(), buffer.size(), error.code) == 0 ? std::string_view{buffer.data()}
                                                                       : std::string_view{"Unknown error"};
#else
    // POSIX reports both categories through errno values.
    return strerror_text(::strerror_r(error.code, buffer.data(), buffer.size()), buffer);
#endif
}

} // namespace neo_cppjieba::detail::platform

// Format error text straight into the diagnostic, so failed checks need no owning string for it.
template <>
struct std::formatter<neo_cppjieba::detail::platform::ErrorMessage> : std::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(const neo_cppjieba::detail::platform::ErrorMessage &error, FormatContext &context) const {
        auto buffer = std::array<char, neo_cppjieba::detail::platform::kErrorMessageCapacity>{};
        return std::formatter<std::string_view>::format(
            neo_cppjieba::detail::platform::error_message_text(error, buffer), context);
    }
};

namespace neo_cppjieba::detail::platform {

// Outcome of one platform call; the error is meaningful only when the call failed.
struct SystemStatus {
    bool succeeded;
    ErrorMessage error;
};

// The status of a platform call that succeeded.
inline constexpr auto kSystemSuccess = SystemStatus{true, ErrorMessage{ErrorCategory::Generic, 0}};

// Capture errno as the status of a CRT or POSIX call; read it immediately after the call.
inline
auto errno_status(bool succeeded) noexcept -> SystemStatus {
    return succeeded ? kSystemSuccess : SystemStatus{false, ErrorMessage{ErrorCategory::Generic, errno}};
}

// Bytes moved by one platform read or write call, and the status that ended it.
struct TransferResult {
    size_t bytes;
    SystemStatus status;
};

// A descriptor opened by a platform call, or -1 with the status of the failed call.
struct DescriptorResult {
    int fd;
    SystemStatus status;
};

#ifdef _WIN32
// Capture GetLastError as the status of a Win32 call; read it immediately after the call.
inline
auto win32_status(BOOL result) noexcept -> SystemStatus {
    return result != 0 ? kSystemSuccess
                       : SystemStatus{false, ErrorMessage{ErrorCategory::System, static_cast<int>(::GetLastError())}};
}

// The Win32 handle behind a CRT descriptor, for APIs that report errors without the CRT invalid-parameter handler.
inline
auto native_handle(int fd) noexcept -> HANDLE {
    return reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
}

// Public file paths are UTF-8; the Windows filesystem API accepts UTF-16.
inline
auto windows_file_path(std::string_view path) -> std::wstring {
    if (path.empty()) {
        return {};
    }
    check(std::in_range<int>(path.size()), "FileBuffer: file path is too long");
    const auto length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
    check(length != 0, "FileBuffer: invalid UTF-8 file path: {}", path);
    auto result = std::wstring(static_cast<size_t>(length), L'\0');
    const auto converted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                                 static_cast<int>(path.size()), result.data(), length);
    check(converted == length, "FileBuffer: failed to convert file path: {}", path);
    return result;
}

// Preserve subsecond write and metadata-change timestamps, which _stat64 does not expose.
inline
auto file_timestamps(int fd, std::string_view path) -> FILE_BASIC_INFO {
    auto info = FILE_BASIC_INFO{};
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    const auto result = ::GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info));
    check(result != 0, "FileBuffer: failed to read file timestamps: {}: {}", path,
          ErrorMessage{ErrorCategory::System, static_cast<int>(::GetLastError())});
    return info;
}
#endif

// Close the owning CRT/POSIX descriptor without retrying a potentially reused descriptor.
inline
auto close_file_descriptor(int fd) noexcept -> int {
#ifdef _WIN32
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}

// Read once into the buffer, capped at the platform's per-call limit and retrying interrupted POSIX reads.
// Zero bytes with a successful status means end of file.
inline
auto read_some(int fd, std::span<char> buffer) noexcept -> TransferResult {
#ifdef _WIN32
    const auto count =
        static_cast<DWORD>(std::min(buffer.size(), static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    auto bytes_read = DWORD{0};
    // ReadFile reports access errors without invoking the CRT invalid-parameter handler.
    const auto result = ::ReadFile(native_handle(fd), buffer.data(), count, &bytes_read, nullptr);
    return {bytes_read, win32_status(result)};
#else
    const auto count = std::min(buffer.size(), static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    auto bytes_read = ::read(fd, buffer.data(), count);
    while (bytes_read == -1 && errno == EINTR) {
        bytes_read = ::read(fd, buffer.data(), count);
    }
    const auto status = errno_status(bytes_read >= 0);
    return {status.succeeded ? static_cast<size_t>(bytes_read) : size_t{0}, status};
#endif
}

// Fill owned storage, retrying interrupted reads and reporting I/O errors or premature EOF through check.
inline
auto read_file_contents(int fd, std::span<char> destination, std::string_view path) -> void {
    assert_check([fd] { return fd >= 0; }, "FileBuffer: invalid internal file descriptor");

    auto offset = size_t{0};
    while (offset < destination.size()) {
        const auto remaining = destination.subspan(offset);
        const auto result = read_some(fd, remaining);
        check(result.status.succeeded, "FileBuffer: failed to read file: {}: {}", path, result.status.error);
        check(result.bytes != 0, "FileBuffer: unexpected EOF (file may have been truncated): {}: read {} of {} bytes",
              path, offset, destination.size());
        assert_check([&] { return result.bytes <= remaining.size(); },
                     "FileBuffer: read exceeded the requested byte count");
        offset += result.bytes;
    }
}

// A null-terminated path in the file system API's encoding, converted once so cleanup needs no allocation.
// kPathSeparators lists the directory separators recognized in UTF-8 file paths.
#ifdef _WIN32
using NativePath = std::wstring;
inline constexpr auto kPathSeparators = std::string_view{"/\\"};
#else
using NativePath = std::string;
inline constexpr auto kPathSeparators = std::string_view{"/"};
#endif

// Convert a UTF-8 path that has no embedded null byte.
inline
auto native_path(std::string_view path) -> NativePath {
    assert_check([&] { return path.find('\0') == std::string_view::npos; }, "FileReplace: unchecked file path");
#ifdef _WIN32
    return windows_file_path(path);
#else
    return std::string{path};
#endif
}

// Open a binary input descriptor without throwing; the caller reports a failed status.
inline
auto open_for_reading(const NativePath &path) noexcept -> DescriptorResult {
#ifdef _WIN32
    const auto fd = ::_wopen(path.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    // Avoid blocking on a FIFO before fstat can reject non-regular files.
    const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
#endif
    return {fd, errno_status(fd != -1)};
}

// Open an independent binary input descriptor; ownership transfers to the caller.
inline
auto open_file(std::string_view path) -> int {
    check(path.find('\0') == std::string_view::npos, "FileBuffer: file path contains an embedded null byte");

    // open(2) requires a null-terminated path
    const auto opened = open_for_reading(native_path(path));
    check(opened.status.succeeded, "FileBuffer: failed to open file: {}: {}", path, opened.status.error);
    return opened.fd;
}

// Store timestamps at their original precision without leaking native stat structures.
struct FileTimestamp {
    std::int64_t seconds;
    std::int64_t nanoseconds;

    auto operator==(const FileTimestamp &) const noexcept -> bool = default;
};

// The file-loading layer compares snapshots of size and modification metadata.
struct FileMetadata {
    std::int64_t size;
    bool regular;
    FileTimestamp modified;
    FileTimestamp changed;

    auto operator==(const FileMetadata &) const noexcept -> bool = default;
};

// Identify which snapshot failed while retaining the existing I/O diagnostic.
enum class FileReadPhase { BeforeRead, AfterRead };

// Metadata of an open descriptor, or the status of the failed stat call.
struct MetadataResult {
    FileMetadata metadata;
    SystemStatus status;
};

// Query size, file type, and timestamps of an open descriptor. Windows reads subsecond timestamps separately and
// reports their failure through check; the path only labels that diagnostic.
inline
auto native_file_metadata(int fd, [[maybe_unused]] std::string_view path) -> MetadataResult {
#ifdef _WIN32
    struct ::_stat64 st{};
    if (::_fstat64(fd, &st) != 0) {
        return {{}, errno_status(false)};
    }
    const auto regular = (st.st_mode & _S_IFMT) == _S_IFREG;
    if (!regular) {
        return {{st.st_size, false, {}, {}}, kSystemSuccess};
    }
    const auto timestamps = file_timestamps(fd, path);
    constexpr auto ticks_per_second = std::int64_t{10000000};
    constexpr auto nanoseconds_per_tick = std::int64_t{100};
    return {{st.st_size,
             true,
             {timestamps.LastWriteTime.QuadPart / ticks_per_second,
              (timestamps.LastWriteTime.QuadPart % ticks_per_second) * nanoseconds_per_tick},
             {timestamps.ChangeTime.QuadPart / ticks_per_second,
              (timestamps.ChangeTime.QuadPart % ticks_per_second) * nanoseconds_per_tick}},
            kSystemSuccess};
#else
    struct ::stat st{};
    if (::fstat(fd, &st) != 0) {
        return {{}, errno_status(false)};
    }
#ifdef __APPLE__
    return {{st.st_size,
             S_ISREG(st.st_mode),
             {st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec},
             {st.st_ctimespec.tv_sec, st.st_ctimespec.tv_nsec}},
            kSystemSuccess};
#else
    return {{st.st_size,
             S_ISREG(st.st_mode),
             {st.st_mtim.tv_sec, st.st_mtim.tv_nsec},
             {st.st_ctim.tv_sec, st.st_ctim.tv_nsec}},
            kSystemSuccess};
#endif
#endif
}

// Capture metadata from an open descriptor; external I/O errors follow the configured check path.
inline
auto file_metadata(int fd, std::string_view path, FileReadPhase phase) -> FileMetadata {
    assert_check([fd] { return fd >= 0; }, "FileBuffer: invalid internal file descriptor");
    const auto result = native_file_metadata(fd, path);
    check(result.status.succeeded, "FileBuffer: failed to stat file{}: {}: {}",
          phase == FileReadPhase::AfterRead ? " after reading" : "", path, result.status.error);
    return result.metadata;
}

/// A file path split for replacement: the prefix up to and including the last separator, the file name after it,
/// and the directory whose entry the replacement changes.
struct FileLocation {
    std::string prefix;
    std::string name;
    std::string directory;
};

// A path without a separator names a file in the current directory.
inline
auto split_file_path(std::string_view path) -> FileLocation {
    const auto split = path.find_last_of(kPathSeparators);
    if (split == std::string_view::npos) {
        return {{}, std::string{path}, "."};
    }
    const auto prefix = path.substr(0, split + 1);
    return {std::string{prefix}, std::string{path.substr(split + 1)}, std::string{prefix}};
}

// Create a file that must not exist yet, for writing; returns -1 with errno set on failure, EEXIST when the name
// is taken. The file gets the default permissions of any newly created file.
inline
auto try_create_new_file(const NativePath &path) -> int {
#ifdef _WIN32
    return ::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
#endif
}

// Write once from the buffer, capped at the platform's per-call limit and retrying interrupted POSIX writes.
inline
auto write_some(int fd, std::span<const std::byte> buffer) noexcept -> TransferResult {
#ifdef _WIN32
    const auto count =
        static_cast<DWORD>(std::min(buffer.size(), static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    auto bytes_written = DWORD{0};
    const auto result = ::WriteFile(native_handle(fd), buffer.data(), count, &bytes_written, nullptr);
    return {bytes_written, win32_status(result)};
#else
    const auto count = std::min(buffer.size(), static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    auto bytes_written = ::write(fd, buffer.data(), count);
    while (bytes_written == -1 && errno == EINTR) {
        bytes_written = ::write(fd, buffer.data(), count);
    }
    const auto status = errno_status(bytes_written >= 0);
    return {status.succeeded ? static_cast<size_t>(bytes_written) : size_t{0}, status};
#endif
}

// Write every byte, retrying interrupted and short writes; I/O errors are reported through check.
inline
auto write_file_contents(int fd, std::span<const std::byte> source, std::string_view path) -> void {
    assert_check([fd] { return fd >= 0; }, "FileReplace: invalid internal file descriptor");

    auto offset = size_t{0};
    while (offset < source.size()) {
        const auto result = write_some(fd, source.subspan(offset));
        check(result.status.succeeded, "FileReplace: failed to write file: {}: {}", path, result.status.error);
        check(result.bytes != 0, "FileReplace: writing made no progress: {}", path);
        offset += result.bytes;
    }
}

// Flush a descriptor's written data to storage, retrying interrupted POSIX syncs.
inline
auto flush_to_storage(int fd) noexcept -> SystemStatus {
#ifdef _WIN32
    return win32_status(::FlushFileBuffers(native_handle(fd)));
#else
#ifdef __APPLE__
    // Apple's fsync leaves data in the drive cache; F_FULLFSYNC flushes it where the file system supports it.
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return kSystemSuccess;
    }
#endif
    auto result = ::fsync(fd);
    while (result == -1 && errno == EINTR) {
        result = ::fsync(fd);
    }
    return errno_status(result == 0);
#endif
}

// Flush written data to storage before the file becomes visible under its final name.
inline
auto sync_file(int fd, std::string_view path) -> void {
    assert_check([fd] { return fd >= 0; }, "FileReplace: invalid internal file descriptor");
    const auto status = flush_to_storage(fd);
    check(status.succeeded, "FileReplace: failed to flush file: {}: {}", path, status.error);
}

// Move a file over a destination, replacing any existing file there; Windows also writes the move through.
inline
auto replace_file(const NativePath &from, const NativePath &to) noexcept -> SystemStatus {
#ifdef _WIN32
    return win32_status(::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
#else
    return errno_status(std::rename(from.c_str(), to.c_str()) == 0);
#endif
}

// Atomically move a file over a destination in the same directory, replacing any existing file there.
// The UTF-8 path only labels diagnostics.
inline
auto rename_over(const NativePath &from, const NativePath &to, std::string_view path) -> void {
    const auto status = replace_file(from, to);
    check(status.succeeded, "FileReplace: failed to replace file: {}: {}", path, status.error);
}

// Persist the directory entry a rename changed; MOVEFILE_WRITE_THROUGH already did so on Windows.
// File systems that cannot sync directories report EINVAL, leaving the rename as durable as they make it.
inline
auto sync_directory(std::string_view directory, std::string_view path) -> void {
#ifdef _WIN32
    static_cast<void>(directory);
    static_cast<void>(path);
#else
    const auto null_terminated_directory = std::string{directory};
    const auto fd = ::open(null_terminated_directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    check(fd != -1, "FileReplace: replaced {} but could not open its directory to sync it: {}", path,
          ErrorMessage{ErrorCategory::Generic, errno});
    const auto result = ::fsync(fd);
    const auto error = errno;
    close_file_descriptor(fd);
    check(result == 0 || error == EINVAL, "FileReplace: replaced {} but failed to sync its directory: {}", path,
          ErrorMessage{ErrorCategory::Generic, error});
#endif
}

// Remove a file during cleanup; failures are ignored because the original error is already being reported.
inline
auto remove_file(const NativePath &path) noexcept -> void {
#ifdef _WIN32
    static_cast<void>(::_wunlink(path.c_str()));
#else
    static_cast<void>(::unlink(path.c_str()));
#endif
}

} // namespace neo_cppjieba::detail::platform
