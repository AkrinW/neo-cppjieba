#pragma once

#include "neo/util/FilePlatform.hpp"
#include "neo/util/Logging.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace neo_cppjieba::detail {

// FileBuffer exposes file contents from owned storage for read-only access.
// It provides an RAII wrapper around a complete read(2) of a regular file.
//
// For dictionary files that are read sequentially once (e.g. jieba.dict.utf8 at ~5MB / 350K lines),
// reading into one buffer avoids the per-line allocation and copying of ifstream/getline.
// The owned buffer is accessed via a single std::string_view,
// and downstream consumers (lines_view, split_view) produce further string_views that point directly
// into that buffer — avoiding further copies during parsing.
//
// Reads proceed sequentially, retrying interruptions and continuing after short reads.
// After loading, the buffer remains valid independently of the file on disk.
//
// Requires a regular file; I/O failures and detected changes during reading throw exceptions.
// Modification checks are best-effort and do not provide an atomic snapshot.
// Publish updates by atomic replacement to avoid mixing file versions during reading.
//
// Usage:
//   auto file = FileBuffer("dict/jieba.dict.utf8");
//   auto content = file.content();  // std::string_view over the entire file
//
// FileBuffer owns a read buffer so external truncation cannot invalidate its views.
// Detected file changes and I/O failures throw; metadata checks do not guarantee an atomic snapshot.
class FileBuffer {
public:
    // Delegation completes the empty object first, so construction failures run its destructor.
    explicit FileBuffer(std::string_view path) : FileBuffer() {
        fd_ = detail::platform::open_file(path);
        const auto before = detail::platform::file_metadata(fd_, path, detail::platform::FileReadPhase::BeforeRead);
        check(before.regular, "FileBuffer: not a regular file: {}", path);
        check(std::in_range<size_t>(before.size) && std::in_range<std::ptrdiff_t>(before.size),
              "FileBuffer: file size is out of range: {}", path);
        size_ = static_cast<size_t>(before.size);

        // Even an empty, successfully loaded file owns storage, distinguishing it from a moved-from object.
        data_.reset(new (std::nothrow) char[std::max(size_, size_t{1})]);
        check(data_ != nullptr, "FileBuffer: failed to allocate {} bytes for file: {}", size_, path);
        detail::platform::read_file_contents(fd_, std::span<char>{data_.get(), size_}, path);

        // Check file size and modification timestamps against the metadata taken before reading.
        // These checks detect changes without retaining any reference to the file-backed pages.
        const auto after = detail::platform::file_metadata(fd_, path, detail::platform::FileReadPhase::AfterRead);
        check(before == after, "FileBuffer: file changed while reading: {}", path);

        // On Linux close releases the descriptor even on EINTR; never retry a possibly reused descriptor.
        const auto close_result = detail::platform::close_file_descriptor(std::exchange(fd_, -1));
        check(close_result == 0, "FileBuffer: failed to close file: {}: {}", path,
              detail::platform::ErrorMessage{detail::platform::ErrorCategory::Generic, errno});
    }

    ~FileBuffer() noexcept {
        // Successful loads close explicitly; exception cleanup must not throw a second exception.
        if (fd_ != -1) {
            detail::platform::close_file_descriptor(fd_);
        }
    }

    // Move-only: transferring ownership of the file buffer.
    // The owned storage keeps its address when moved.
    FileBuffer(FileBuffer &&other) noexcept
        : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)), fd_(std::exchange(other.fd_, -1)) {
    }

    auto operator=(FileBuffer &&other) noexcept -> FileBuffer & {
        if (this != &other) {
            if (fd_ != -1) {
                detail::platform::close_file_descriptor(fd_);
            }
            data_ = std::move(other.data_);
            size_ = std::exchange(other.size_, 0);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    FileBuffer(const FileBuffer &) = delete;
    auto operator=(const FileBuffer &) -> FileBuffer & = delete;

    // content returns a string_view spanning the entire file buffer.
    // The view remains valid while this object or its move destination owns the buffer.
    // Move assignment invalidates views into the destination's previous buffer.
    [[nodiscard]]
    auto content() const noexcept -> std::string_view {
        return size_ == 0 ? std::string_view{} : std::string_view{data_.get(), size_};
    }

    // size returns the file size in bytes.
    [[nodiscard]]
    auto size() const noexcept -> size_t {
        return size_;
    }

    // is_open reports whether this object owns successfully loaded file content.
    // An empty file is valid, while a moved-from object is not; the descriptor is already closed.
    [[nodiscard]]
    auto is_open() const noexcept -> bool {
        return data_ != nullptr;
    }

private:
    FileBuffer() = default;

    std::unique_ptr<char[]> data_;
    size_t size_{0};
    int fd_{-1};
};

// read_file reads the given file path into a FileBuffer.
// File failures are reported through check before returning any content.
inline
auto read_file(std::string_view path) -> FileBuffer {
    return FileBuffer{path};
}

// ReplacementFile is a new file created beside a destination for an atomic replacement. It is removed on
// destruction unless publish() renamed it over the destination, so failures never leave it behind.
// A process that dies before the rename can still leave one; its name starts with '.' and ends with ".tmp".
class ReplacementFile {
public:
    explicit ReplacementFile(const detail::platform::FileLocation &destination) {
        auto random = std::random_device{};
        for (auto attempt = 0; attempt < 64; ++attempt) {
            const auto suffix = (static_cast<std::uint64_t>(random()) << 32) | random();
            path_ = temporary_path(destination, suffix);
            native_ = detail::platform::native_path(path_);
            fd_ = detail::platform::try_create_new_file(native_);
            if (fd_ != -1) {
                return;
            }
            const auto error = errno;
            check(error == EEXIST, "FileReplace: failed to create a temporary file: {}: {}", path_,
                  detail::platform::ErrorMessage{detail::platform::ErrorCategory::Generic, error});
        }
        check(false, "FileReplace: found no unused temporary file name beside {}", destination.name);
    }

    ~ReplacementFile() noexcept {
        if (fd_ != -1) {
            detail::platform::close_file_descriptor(fd_);
        }
        if (!native_.empty()) {
            detail::platform::remove_file(native_);
        }
    }

    ReplacementFile(const ReplacementFile &) = delete;
    auto operator=(const ReplacementFile &) -> ReplacementFile & = delete;

    // Write the bytes, flush them to storage and close the file, then rename it over destination.
    // path names the destination in diagnostics.
    auto publish(std::span<const std::byte> bytes, const detail::platform::NativePath &destination,
                 std::string_view path) -> void {
        detail::platform::write_file_contents(fd_, bytes, path_);
        detail::platform::sync_file(fd_, path_);
        // On Linux close releases the descriptor even on EINTR; never retry a possibly reused descriptor.
        const auto close_result = detail::platform::close_file_descriptor(std::exchange(fd_, -1));
        check(close_result == 0, "FileReplace: failed to close file: {}: {}", path_,
              detail::platform::ErrorMessage{detail::platform::ErrorCategory::Generic, errno});
        detail::platform::rename_over(native_, destination, path);
        native_.clear(); // The file now lives under the destination name.
    }

private:
    // A hidden name beside the destination, so the rename stays within one directory and one file system.
    static
    auto temporary_path(const detail::platform::FileLocation &destination, std::uint64_t suffix) -> std::string {
        auto digits = std::array<char, 16>{};
        auto *const end = std::to_chars(digits.data(), digits.data() + digits.size(), suffix, 16).ptr;
        return destination.prefix + "." + destination.name + "." + std::string{digits.data(), end} + ".tmp";
    }

    std::string path_;
    detail::platform::NativePath native_;
    int fd_{-1};
};

// replace_file_atomically replaces the file at path with bytes so readers see the previous file or the complete
// new one, never a partial write. The bytes reach storage under a temporary name beside path before a rename
// publishes them; a failure before the rename leaves path untouched. The new file gets default permissions.
inline
auto replace_file_atomically(std::string_view path, std::span<const std::byte> bytes) -> void {
    check(path.find('\0') == std::string_view::npos, "FileReplace: file path contains an embedded null byte");
    const auto location = detail::platform::split_file_path(path);
    check(!location.name.empty(), "FileReplace: file path has no file name: {}", path);
    const auto destination = detail::platform::native_path(path);
    auto file = ReplacementFile{location};
    file.publish(bytes, destination, path);
    detail::platform::sync_directory(location.directory, path);
}

} // namespace neo_cppjieba::detail
