#pragma once

/// Shared utilities for neo unit tests:
///   - to_strings : convert WordRange vector to UTF-8 string vector
///   - join       : concatenate strings with a separator

#include "neo/Utf.hpp"

#include <cstddef>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Create an isolated directory using an atomic filesystem operation on every platform.
inline
auto create_temp_directory(std::string_view prefix) -> std::filesystem::path {
    const auto root = std::filesystem::temp_directory_path();
    auto random = std::random_device{};
    for (auto attempt = 0; attempt < 64; ++attempt) {
        const auto path = root / (std::string{prefix} + std::to_string(random()));
        auto error = std::error_code{};
        if (std::filesystem::create_directory(path, error)) {
            return path;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error{"create test directory", path, error};
        }
    }
    throw std::filesystem::filesystem_error{"create unique test directory", root,
                                            std::make_error_code(std::errc::file_exists)};
}

// Pass UTF-8 paths to the library even when the native filesystem uses UTF-16.
inline
auto path_to_utf8(const std::filesystem::path &path) -> std::string {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}

/// Convert a vector of WordRange to a vector of UTF-8 strings for easy comparison.
inline
auto to_strings(std::span<const neo_cppjieba::Rune> runes, const std::vector<neo_cppjieba::WordRange> &ranges)
    -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    result.reserve(ranges.size());
    for (auto &&r : ranges) {
        result.push_back(neo_cppjieba::Utf::encode<char>(r.slice(runes)));
    }
    return result;
}

/// Join a vector of strings with a separator for display.
inline
auto join(const std::vector<std::string> &v, std::string_view sep = "/") -> std::string {
    auto result = std::string{};
    for (auto i = size_t{0}; i < v.size(); ++i) {
        if (i > 0) {
            result.append(sep);
        }
        result.append(v[i]);
    }
    return result;
}
