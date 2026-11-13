#pragma once

#include "neo/util/Logging.hpp"

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
namespace neo_cppjieba::detail {

// Dictionary, model and IDF fields hold integers and floating-point numbers, parsed with from_chars.
template <typename T>
concept TextNumber = (std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>;

// decode_value is a helper function that decodes a value of type T from a string_view using std::from_chars.
// it add error checking and throws an exception if parsing fails.
template <TextNumber T>
constexpr
auto decode_value(std::string_view sv) -> T {
    check(!sv.empty(), "cannot parse a value from an empty string");
    auto val = T{};
    auto &&[ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    check(ec == std::errc{} && ptr == sv.data() + sv.size(), "failed to parse value from '{}'", sv);
    return val;
}

// encode_value is a helper function that converts a value of type T to a string using std::to_chars.
template <TextNumber T>
constexpr
auto encode_value(const T &val) -> std::string {
    auto buf = std::array<char, 32>{};
    auto &&[ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), val);
    check(ec == std::errc{}, "failed to format value as string");
    return std::string{buf.data(), static_cast<size_t>(ptr - buf.data())};
}

// lines_view is a lazy, zero-copy range that splits a string_view by newline characters.
//
// Each dereferenced iterator yields a std::string_view pointing directly into the original data.
// Both '\n' and '\r\n' line endings are handled (the '\r' is stripped from the view).
// A trailing newline does NOT produce an extra empty element — i.e. "a\nb\n" yields {"a", "b"}.
//
// Iteration pattern: a default-constructed iterator acts as the end sentinel.
//
// Usage:
//   for (auto line : lines_view{file.content()}) {
//       // line is a string_view — no allocation, no copy
//   }
//
class lines_view {
public:
    // Compact iterator — 3 raw pointers (24 bytes on 64-bit, down from 40).
    // The current line is [cur_, ...) with the end computed on dereference from next_.
    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::string_view;

        // Default-constructed iterator represents the end (sentinel).
        constexpr iterator() noexcept = default;

        constexpr
        explicit iterator(std::string_view content) noexcept
            : next_(content.data()), end_(content.data() + content.size()) {
            advance();
        }

        // Compute the current line on dereference: strip trailing \n (and \r\n) from [cur_, next_).
        constexpr
        auto operator*() const noexcept -> std::string_view {
            assert_check([this] { return cur_ != nullptr && next_ > cur_ && next_ <= end_; },
                         "lines_view: cannot dereference an exhausted or invalid iterator");
            const auto *e = next_;
            if (*(e - 1) == '\n') {
                --e;
                if (e > cur_ && *(e - 1) == '\r') {
                    --e;
                }
            }
            return std::string_view{cur_, static_cast<size_t>(e - cur_)};
        }

        constexpr
        auto operator++() noexcept -> iterator & {
            advance();
            return *this;
        }

        constexpr
        auto operator++(int) noexcept -> iterator {
            auto tmp = *this;
            advance();
            return tmp;
        }

        constexpr
        auto operator==(const iterator &other) const noexcept -> bool {
            return cur_ == other.cur_;
        }

    private:
        constexpr
        auto advance() noexcept -> void {
            if (next_ == nullptr) {
                cur_ = nullptr;
                return;
            }
            cur_ = next_;
            if (cur_ == end_) {
                cur_ = nullptr;
                return;
            }
            const auto *p = cur_;
            while (p != end_ && *p != '\n') {
                ++p;
            }
            next_ = (p != end_) ? p + 1 : end_;
        }

        const char *cur_{nullptr};  // start of current line; nullptr = end sentinel
        const char *next_{nullptr}; // one past '\n' of current line, or end_
        const char *end_{nullptr};  // end of all data
    };

    constexpr lines_view() noexcept = default;
    constexpr
    explicit lines_view(std::string_view content) noexcept
        : content_(content) {
    }

    [[nodiscard]] constexpr
    auto begin() const noexcept -> iterator {
        return iterator{content_};
    }
    [[nodiscard]] constexpr
    auto end() const noexcept -> iterator {
        return iterator{};
    }

private:
    std::string_view content_{};
};

// get_line_view returns a lazy zero-copy range of lines over the given content.
constexpr
auto get_line_view(std::string_view content) noexcept -> lines_view {
    return lines_view{content};
}

// split_view is a lazy, zero-copy range that splits a string_view by a single-character delimiter.
//
// Consecutive delimiters produce empty elements (standard split semantics).
// For dictionary lines like "AT&T 3 nz", splitting by ' ' yields {"AT&T", "3", "nz"}.
//
// Iteration pattern: a default-constructed iterator acts as the end sentinel.
//
// Usage:
//   for (auto field : split_view{line, ' '}) {
//       // field is a string_view — no allocation, no copy
//   }
//
class split_view {
public:
    // Compact iterator — 3 raw pointers + 1 char (32 bytes on 64-bit, down from 40).
    // The current field is computed on dereference from [cur_, next_field_-1) or [cur_, data_end_).
    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::string_view;

        // Default-constructed iterator represents the end (sentinel).
        constexpr iterator() noexcept = default;

        constexpr iterator(std::string_view content, char delim) noexcept
            : next_field_(content.data()), data_end_(content.data() + content.size()), delim_(delim) {
            advance();
        }

        // Compute the current field on dereference.
        // If next_field_ is nullptr (last field), end = data_end_; otherwise end = next_field_ - 1.
        constexpr
        auto operator*() const noexcept -> std::string_view {
            assert_check([this] { return cur_ != nullptr && cur_ <= data_end_; },
                         "split_view: cannot dereference an exhausted or invalid iterator");
            assert_check([this] { return next_field_ == nullptr || (cur_ < next_field_ && next_field_ <= data_end_); },
                         "split_view: invalid next field boundary");
            const auto *end = (next_field_ == nullptr) ? data_end_ : next_field_ - 1;
            return std::string_view{cur_, static_cast<size_t>(end - cur_)};
        }

        constexpr
        auto operator++() noexcept -> iterator & {
            advance();
            return *this;
        }

        constexpr
        auto operator++(int) noexcept -> iterator {
            auto tmp = *this;
            advance();
            return tmp;
        }

        constexpr
        auto operator==(const iterator &other) const noexcept -> bool {
            return cur_ == other.cur_;
        }

    private:
        constexpr
        auto advance() noexcept -> void {
            cur_ = next_field_;
            if (cur_ == nullptr) {
                return;
            }
            const auto *p = cur_;
            while (p != data_end_ && *p != delim_) {
                ++p;
            }
            next_field_ = (p != data_end_) ? p + 1 : nullptr;
        }

        const char *cur_{nullptr};        // start of current field; nullptr = end sentinel
        const char *next_field_{nullptr}; // start of next field; nullptr = last field consumed
        const char *data_end_{nullptr};   // end of all data
        char delim_ = ' ';
    };

    constexpr split_view() noexcept = default;
    constexpr split_view(std::string_view content, char delim) noexcept : content_(content), delim_(delim) {
    }

    [[nodiscard]] constexpr
    auto begin() const noexcept -> iterator {
        return iterator{content_, delim_};
    }
    [[nodiscard]] constexpr
    auto end() const noexcept -> iterator {
        return iterator{};
    }

private:
    std::string_view content_;
    char delim_ = ' ';
};

// get_split_view returns a lazy zero-copy range of tokens from content, split by delim.
constexpr
auto get_split_view(std::string_view content, char delim) noexcept -> split_view {
    return split_view{content, delim};
}

// trim is a helper function that removes leading and trailing whitespace from a string_view.
constexpr
auto trim(std::string_view sv) noexcept -> std::string_view {
    auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    auto end = sv.find_last_not_of(" \t\r\n");
    return sv.substr(start, end - start + 1);
}

} // namespace neo_cppjieba::detail
