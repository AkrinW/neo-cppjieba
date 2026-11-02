#pragma once

/// Shared helpers for C ABI harnesses:
///   - modes, tiles_input : the cut modes and which of them tile their input
///   - repository_engine  : the repository dictionaries, loaded once per process
///   - ErrorProbe : diagnostic storage that checks the error-buffer contract of each call
///   - borrowed : view a borrowed C array, which is NULL exactly when it is empty

#include "FuzzUtils.hpp"
#include "neo_cppjieba.h"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using EngineOwner = std::unique_ptr<neo_jieba_engine_v1, decltype(&neo_jieba_v1_engine_destroy)>;
using TokensOwner = std::unique_ptr<neo_jieba_tokens_v1, decltype(&neo_jieba_v1_tokens_destroy)>;
using WorkspaceOwner = std::unique_ptr<neo_jieba_workspace_v1, decltype(&neo_jieba_v1_workspace_destroy)>;
using RangesOwner = std::unique_ptr<neo_jieba_ranges_v1, decltype(&neo_jieba_v1_ranges_destroy)>;
using KeywordsOwner = std::unique_ptr<neo_jieba_keywords_v1, decltype(&neo_jieba_v1_keywords_destroy)>;

inline constexpr auto modes = std::array<neo_jieba_mode_v1, 7>{
    NEO_JIEBA_V1_MIX,           NEO_JIEBA_V1_MIX_NO_HMM, NEO_JIEBA_V1_FULL, NEO_JIEBA_V1_SEARCH,
    NEO_JIEBA_V1_SEARCH_NO_HMM, NEO_JIEBA_V1_HMM,        NEO_JIEBA_V1_MP};

// Full and search modes may emit overlapping ranges; every other mode tiles the input with adjacent ranges.
inline auto tiles_input(neo_jieba_mode_v1 mode) -> bool {
    return mode != NEO_JIEBA_V1_FULL && mode != NEO_JIEBA_V1_SEARCH && mode != NEO_JIEBA_V1_SEARCH_NO_HMM;
}

// Load the repository dictionaries once; libFuzzer runs every input in this process.
inline auto repository_engine() -> const neo_jieba_engine_v1 * {
    static const auto *const owned = [] {
        const auto paths = neo_jieba_paths_v1{DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8", ""};
        neo_jieba_engine_v1 *created = nullptr;
        require(neo_jieba_v1_engine_create(&paths, &created, nullptr) == NEO_JIEBA_V1_OK, "engine_create failed");
        return created;
    }();
    return owned;
}

// Describe borrowed contents as a C memory source.
inline auto memory_source(std::string_view text) -> neo_jieba_source_v1 {
    return {NEO_JIEBA_V1_SOURCE_MEMORY, 0, text.data(), text.size()};
}

// Describe a NUL-free path as a C file source; the path text must outlive the call that reads it.
inline auto file_source(std::string_view path) -> neo_jieba_source_v1 {
    return {NEO_JIEBA_V1_SOURCE_FILE, 0, path.data(), path.size()};
}

// View a borrowed C array; the library describes an empty array with NULL and a nonempty one with storage.
template <typename T>
auto borrowed(const T *data, std::uint64_t count) -> std::span<const T> {
    require((data == nullptr) == (count == 0), "borrowed array pointer disagrees with its count");
    return count == 0 ? std::span<const T>{} : std::span{data, static_cast<std::size_t>(count)};
}

// A tag is NUL-padded when no byte follows its first NUL.
inline auto is_padded_tag(const neo_jieba_pos_tag_v1 &tag) -> bool {
    return std::all_of(std::ranges::find(tag.data, '\0'), std::ranges::end(tag.data),
                       [](char byte) { return byte == '\0'; });
}

// Tags compare by all four bytes, so padding differences count.
inline auto same_tag(const neo_jieba_pos_tag_v1 &left, const neo_jieba_pos_tag_v1 &right) -> bool {
    return std::ranges::equal(left.data, right.data);
}

// Diagnostic storage with guard bytes past its capacity; a selector byte picks the capacity, including none.
class ErrorProbe {
public:
    explicit ErrorProbe(std::uint8_t selector)
        : capacity_{capacities[selector % capacities.size()]}, storage_(capacity_ + guard_size, sentinel) {
    }

    // Reset before each call so a message left by an earlier call cannot satisfy the next check.
    auto buffer() -> neo_jieba_error_buffer_v1 * {
        std::ranges::fill(storage_, sentinel);
        error_ = {capacity_ == 0 ? nullptr : storage_.data(), capacity_, unset_size};
        return &error_;
    }

    // Successful calls clear the message; failures report its length and a NUL-terminated prefix within capacity.
    auto check(neo_jieba_status_v1 status) const -> void {
        require(status >= NEO_JIEBA_V1_OK && status <= NEO_JIEBA_V1_LIMIT_EXCEEDED, "undocumented status");
        if (status == NEO_JIEBA_V1_OK) {
            require(error_.message_size == 0, "a successful call left a message");
            return;
        }
        require(error_.message_size != unset_size && error_.message_size > 0, "a failed call reported no message");
        if (capacity_ > 0) {
            const auto end = std::min(error_.message_size, static_cast<std::uint64_t>(capacity_ - 1));
            require(storage_[static_cast<std::size_t>(end)] == '\0', "the message prefix is not NUL-terminated");
        }
        require(std::ranges::all_of(std::span{storage_}.subspan(capacity_), [](char byte) { return byte == sentinel; }),
                "a message was written past the buffer capacity");
    }

private:
    static constexpr auto capacities = std::array<std::size_t, 5>{0, 1, 2, 16, 256};
    static constexpr auto guard_size = std::size_t{8};
    static constexpr auto sentinel = '\x7F';
    static constexpr auto unset_size = std::numeric_limits<std::uint64_t>::max();

    std::size_t capacity_;
    std::vector<char> storage_;
    neo_jieba_error_buffer_v1 error_{};
};

// Create a reusable workspace; creation only fails when memory runs out.
inline auto make_workspace() -> WorkspaceOwner {
    neo_jieba_workspace_v1 *created = nullptr;
    require(neo_jieba_v1_workspace_create(&created, nullptr) == NEO_JIEBA_V1_OK, "workspace_create failed");
    return {created, neo_jieba_v1_workspace_destroy};
}

// Create an empty reusable ranges result; creation only fails when memory runs out.
inline auto make_ranges() -> RangesOwner {
    neo_jieba_ranges_v1 *created = nullptr;
    require(neo_jieba_v1_ranges_create(&created, nullptr) == NEO_JIEBA_V1_OK, "ranges_create failed");
    return {created, neo_jieba_v1_ranges_destroy};
}

// Cut input into an owned result and copy its ranges; the caller has checked that input is valid UTF-8.
inline auto owned_ranges(const neo_jieba_engine_v1 *engine, std::string_view input, neo_jieba_mode_v1 mode)
    -> std::vector<neo_jieba_token_range_v1> {
    neo_jieba_tokens_v1 *created = nullptr;
    require(neo_jieba_v1_cut_utf8_owned(engine, input.data(), input.size(), mode, &created, nullptr) == NEO_JIEBA_V1_OK,
            "valid UTF-8 was not cut");
    const auto tokens = TokensOwner{created, neo_jieba_v1_tokens_destroy};
    auto data = neo_jieba_borrowed_tokens_v1{};
    require(neo_jieba_v1_tokens_borrow_data(tokens.get(), &data, nullptr) == NEO_JIEBA_V1_OK,
            "tokens_borrow_data failed");
    const auto ranges = borrowed(data.ranges, data.count);
    return {ranges.begin(), ranges.end()};
}

// Ranges compare by offset and length.
inline auto same_range(const neo_jieba_token_range_v1 &left, const neo_jieba_token_range_v1 &right) -> bool {
    return left.byte_offset == right.byte_offset && left.byte_length == right.byte_length;
}
