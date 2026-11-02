#pragma once

/// Shared helpers for libFuzzer targets:
///   - require : abort when a harness property fails, so libFuzzer keeps the input
///   - as_text : view fuzz bytes as UTF-8 text without copying
///   - ByteReader, split_parts : decode structured requests from fuzz bytes
///   - small_dict, small_model : tiny sources for harnesses that build or clone an engine per input

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

// Abort with a message so libFuzzer records the input that broke a harness property.
inline auto require(bool condition, std::string_view property) -> void {
    if (!condition) {
        std::fprintf(stderr, "fuzz property violated: %.*s\n", static_cast<int>(property.size()), property.data());
        std::abort();
    }
}

// View fuzz bytes as UTF-8 text; libFuzzer keeps them alive for the whole callback.
inline auto as_text(const std::uint8_t *data, std::size_t size) -> std::string_view {
    return size == 0 ? std::string_view{} : std::string_view{reinterpret_cast<const char *>(data), size};
}

// Consume fuzz bytes from the front; reads past the end yield zeros, so every input decodes to some request.
class ByteReader {
public:
    ByteReader(const std::uint8_t *data, std::size_t size) : rest_{as_text(data, size)} {
    }

    auto byte() -> std::uint8_t {
        if (rest_.empty()) {
            return 0;
        }
        const auto value = static_cast<std::uint8_t>(rest_.front());
        rest_.remove_prefix(1);
        return value;
    }

    auto text(std::size_t size) -> std::string_view {
        const auto taken = rest_.substr(0, size);
        rest_.remove_prefix(taken.size());
        return taken;
    }

    auto rest() -> std::string_view {
        return text(rest_.size());
    }

    [[nodiscard]] auto empty() const -> bool {
        return rest_.empty();
    }

private:
    std::string_view rest_;
};

// Split text at 0xFF bytes, which never occur in valid UTF-8, into at most max_parts parts; the last keeps the rest.
inline auto split_parts(std::string_view text, std::size_t max_parts) -> std::vector<std::string_view> {
    auto parts = std::vector<std::string_view>{};
    while (parts.size() + 1 < max_parts) {
        const auto end = text.find('\xFF');
        if (end == std::string_view::npos) {
            break;
        }
        parts.push_back(text.substr(0, end));
        text.remove_prefix(end + 1);
    }
    parts.push_back(text);
    return parts;
}

// A tiny main dictionary keeps per-input engine builds and clones cheap.
inline constexpr auto small_dict =
    std::string_view{"北京 2000 ns\n清华大学 800 nt\n大学 1500 n\n来到 1200 v\n我 9000 r\n"};

// CppJieba's start and transition rows with emissions for a few characters; others take the model's floor.
inline constexpr auto small_model = std::string_view{"-0.26268660809250016 -3.14e+100 -3.14e+100 -1.4652633398537678\n"
                                                     "-3.14e+100 -0.510825623765990 -0.916290731874155 -3.14e+100\n"
                                                     "-0.5897149736854513 -3.14e+100 -3.14e+100 -0.8085250474669937\n"
                                                     "-3.14e+100 -0.33344856811948514 -1.2603623820268226 -3.14e+100\n"
                                                     "-0.7211965654669841 -3.14e+100 -3.14e+100 -0.6658631448798212\n"
                                                     "北:-8.4,京:-10.1,大:-6.9,学:-9.3\n"
                                                     "北:-10.6,京:-8.2,大:-9.1,学:-7.4\n"
                                                     "北:-11.2,京:-10.4,大:-8.8,学:-9.9\n"
                                                     "北:-9.7,京:-9.6,大:-7.2,学:-11.3\n"};
