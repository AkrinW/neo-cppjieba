#pragma once

#include "neo/Config.hpp"
#include "neo/concept/Text.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace neo_cppjieba {

namespace detail {

// Encoding represents the encoding type of a string.
enum class Encoding : uint8_t { UTF8, UTF16, UTF32 };

// encoding_of is a type trait that maps a character type to its corresponding encoding.
template <typename T>
struct encoding_of;

// Byte storage uses the same UTF-8 decoder as char and char8_t.
template <ByteType T>
struct encoding_of<T> {
    static constexpr auto value = Encoding::UTF8;
};

template <>
struct encoding_of<char> {
    static constexpr auto value = Encoding::UTF8;
};

template <>
struct encoding_of<char8_t> {
    static constexpr auto value = Encoding::UTF8;
};

template <>
struct encoding_of<char16_t> {
    static constexpr auto value = Encoding::UTF16;
};

template <>
struct encoding_of<char32_t> {
    static constexpr auto value = Encoding::UTF32;
};

// Wide input must contain UTF-16 or UTF-32 code units according to its width, independently of the locale.
template <>
struct encoding_of<wchar_t> {
    static_assert(sizeof(wchar_t) == 2 || sizeof(wchar_t) == 4, "Unsupported wchar_t width");
    static constexpr auto value = sizeof(wchar_t) == 2 ? Encoding::UTF16 : Encoding::UTF32;
};

// encoding_of_v is a helper variable template that provides a convenient way to access the encoding type for a given
// character type.
template <typename T>
    requires requires { encoding_of<std::remove_cvref_t<T>>::value; }
inline constexpr auto encoding_of_v = encoding_of<std::remove_cvref_t<T>>::value;

// Unified input adaptation for as_code_units and as_view.
// Character arrays omit one trailing NUL; byte buffers and sized ranges retain every code unit, including NULs.
// Pointer inputs must refer to a readable, NUL-terminated C string. (Former contract; use a sized view now.)
template <StringViewSource T>
constexpr auto as_code_units(T &&input) -> std::span<const resolve_char_type_t<T>> {
    using CharT = resolve_char_type_t<T>;
    const auto &source = input;

    // NOLINTNEXTLINE(bugprone-branch-clone): byte arrays keep every element, so they precede the array branch.
    if constexpr (ByteType<CharT>) {
        return {std::ranges::data(source), std::ranges::size(source)};
    } else if constexpr (std::is_array_v<std::remove_reference_t<T>>) {
        constexpr auto extent = std::extent_v<std::remove_reference_t<T>>;
        const auto size = extent - (source[extent - 1] == CharT{} ? 1 : 0);
        return {source, size};
    } else if constexpr (ConstCodeUnitRange<T>) {
        // Branch B: sized strings, views, std::vector<CharT>, std::array<CharT, N>, std::span<CharT>, etc.
        return {std::ranges::data(source), std::ranges::size(source)};
    } else {
        // Branch A: custom types with a const conversion to string_view.
        const auto view = std::basic_string_view<CharT>{source};
        return {view.data(), view.size()};
    }
}

// Expose a string view without copying; char may inspect the representation of any byte type.
// Byte-to-char views require runtime evaluation in C++23; as_code_units also supports constant evaluation.
template <StringViewSource T>
constexpr auto as_view(T &&input) -> std::basic_string_view<output_char_type_t<T>> {
    const auto units = as_code_units(std::forward<T>(input));
    if constexpr (ByteType<resolve_char_type_t<T>>) {
        return {reinterpret_cast<const char *>(units.data()), units.size()};
    } else {
        return {units.data(), units.size()};
    }
}

// Distinguish malformed input from a successful decoding of the NUL character.
enum class UnicodeDecodeError : uint8_t {
    None,
    EmptyInput,
    TruncatedSequence,
    InvalidLeadingUnit,
    InvalidContinuation,
    OverlongSequence,
    Surrogate,
    OutOfRange
};

// One decoded scalar and its consumed source code units, or a specific input error.
struct DecodedRune {
    Rune rune{};
    uint8_t code_units{};
    UnicodeDecodeError error{UnicodeDecodeError::None};
};

// Exclude surrogate code points while retaining NUL, noncharacters and unassigned scalars.
[[nodiscard]] constexpr auto is_unicode_scalar(Rune rune) noexcept -> bool {
    return rune <= 0x10FFFF && (rune < 0xD800 || rune > 0xDFFF);
}

// Describe the input failure without allocating inside the decoding kernel.
constexpr auto decode_error_name(UnicodeDecodeError error) noexcept -> std::string_view {
    switch (error) {
        case UnicodeDecodeError::None:
            return "none";
        case UnicodeDecodeError::EmptyInput:
            return "empty input";
        case UnicodeDecodeError::TruncatedSequence:
            return "truncated sequence";
        case UnicodeDecodeError::InvalidLeadingUnit:
            return "invalid leading code unit";
        case UnicodeDecodeError::InvalidContinuation:
            return "invalid continuation code unit";
        case UnicodeDecodeError::OverlongSequence:
            return "overlong sequence";
        case UnicodeDecodeError::Surrogate:
            return "surrogate code point";
        case UnicodeDecodeError::OutOfRange:
            return "code point exceeds U+10FFFF";
    }
    assert_check([] { return false; }, "Unknown internal Unicode decoding error");
    std::unreachable();
}

// Report external decoding failures consistently at the public entry and within the decoding loop.
template <CodeUnit CharT>
inline auto throw_decode_error(UnicodeDecodeError error, size_t offset) -> void {
    constexpr auto encoding = encoding_of_v<CharT> == Encoding::UTF8    ? "UTF-8"
                              : encoding_of_v<CharT> == Encoding::UTF16 ? "UTF-16"
                                                                        : "UTF-32";
    check(false, "{} decoding failed at code-unit offset {}: {}", encoding, offset, decode_error_name(error));
}

// decode_one_utf8 decodes a single Unicode code point from a UTF-8 encoded string and returns it as a Rune.
template <CodeUnit CharT>
    requires(encoding_of_v<CharT> == Encoding::UTF8)
constexpr auto decode_one_utf8(std::span<const CharT> input) noexcept -> DecodedRune {
    assert_check([&] { return !input.empty(); }, "UTF-8 decoding requires a nonempty internal span");
    const auto first = static_cast<uint8_t>(input[0]);
    // one byte (ASCII)
    if (first < 0x80) {
        return {.rune = static_cast<Rune>(first), .code_units = 1};
    }
    // two bytes
    if (first >> 5 == 0x6) {
        if (input.size() < 2) {
            return {.error = UnicodeDecodeError::TruncatedSequence};
        }
        const auto second = static_cast<uint8_t>(input[1]);
        if (second >> 6 != 0x2) {
            return {.error = UnicodeDecodeError::InvalidContinuation};
        }
        const auto rune = static_cast<Rune>(((first & 0x1F) << 6) | (second & 0x3F));
        if (rune < 0x80) {
            return {.error = UnicodeDecodeError::OverlongSequence};
        }
        return {.rune = rune, .code_units = 2};
    }
    // three bytes
    if (first >> 4 == 0xE) {
        if (input.size() < 3) {
            return {.error = UnicodeDecodeError::TruncatedSequence};
        }
        const auto second = static_cast<uint8_t>(input[1]);
        const auto third = static_cast<uint8_t>(input[2]);
        if (second >> 6 != 0x2 || third >> 6 != 0x2) {
            return {.error = UnicodeDecodeError::InvalidContinuation};
        }
        const auto rune = static_cast<Rune>(((first & 0xF) << 12) | ((second & 0x3F) << 6) | (third & 0x3F));
        if (rune < 0x800) {
            return {.error = UnicodeDecodeError::OverlongSequence};
        }
        if (!is_unicode_scalar(rune)) {
            return {.error = UnicodeDecodeError::Surrogate};
        }
        return {.rune = rune, .code_units = 3};
    }
    // four bytes
    if (first < 0xF0 || first > 0xF4) {
        return {.error = UnicodeDecodeError::InvalidLeadingUnit};
    }
    if (input.size() < 4) {
        return {.error = UnicodeDecodeError::TruncatedSequence};
    }
    const auto second = static_cast<uint8_t>(input[1]);
    const auto third = static_cast<uint8_t>(input[2]);
    const auto fourth = static_cast<uint8_t>(input[3]);
    if (second >> 6 != 0x2 || third >> 6 != 0x2 || fourth >> 6 != 0x2) {
        return {.error = UnicodeDecodeError::InvalidContinuation};
    }
    const auto rune =
        static_cast<Rune>(((first & 0x7) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F));
    if (rune < 0x10000) {
        return {.error = UnicodeDecodeError::OverlongSequence};
    }
    if (!is_unicode_scalar(rune)) {
        return {.error = UnicodeDecodeError::OutOfRange};
    }
    return {.rune = rune, .code_units = 4};
}

// decode_one_utf16 decodes a single Unicode code point from a UTF-16 encoded string and returns it as a Rune.
template <CharType CharT>
    requires(encoding_of_v<CharT> == Encoding::UTF16)
constexpr auto decode_one_utf16(std::span<const CharT> input) noexcept -> DecodedRune {
    assert_check([&] { return !input.empty(); }, "UTF-16 decoding requires a nonempty internal span");
    const auto first = static_cast<uint16_t>(input[0]);
    // single code unit (BMP)
    if (first < 0xD800 || first > 0xDFFF) {
        return {.rune = static_cast<Rune>(first), .code_units = 1};
    }
    // surrogate pair
    if (first > 0xDBFF) {
        return {.error = UnicodeDecodeError::Surrogate};
    }
    if (input.size() < 2) {
        return {.error = UnicodeDecodeError::TruncatedSequence};
    }
    const auto second = static_cast<uint16_t>(input[1]);
    if (second < 0xDC00 || second > 0xDFFF) {
        return {.error = UnicodeDecodeError::InvalidContinuation};
    }
    return {.rune = static_cast<Rune>(((first - 0xD800) << 10) | (second - 0xDC00)) + 0x10000, .code_units = 2};
}

// decode_one_utf32 decodes a single Unicode code point from a UTF-32 encoded string and returns it as a Rune.
template <CharType CharT>
    requires(encoding_of_v<CharT> == Encoding::UTF32)
constexpr auto decode_one_utf32(std::span<const CharT> input) noexcept -> DecodedRune {
    assert_check([&] { return !input.empty(); }, "UTF-32 decoding requires a nonempty internal span");
    const auto rune = static_cast<Rune>(input[0]);
    if (rune > 0x10FFFF) {
        return {.error = UnicodeDecodeError::OutOfRange};
    }
    if (!is_unicode_scalar(rune)) {
        return {.error = UnicodeDecodeError::Surrogate};
    }
    return {.rune = rune, .code_units = 1};
}

// Dispatch by encoding without reinterpreting pointers to distinct character types.
template <CodeUnit CharT>
constexpr auto decode_step(std::span<const CharT> input) noexcept -> DecodedRune {
    if constexpr (encoding_of_v<CharT> == Encoding::UTF8) {
        return decode_one_utf8(input);
    } else if constexpr (encoding_of_v<CharT> == Encoding::UTF16) {
        return decode_one_utf16(input);
    } else {
        return decode_one_utf32(input);
    }
}

// Keep exception construction off the successful path, including constant evaluation.
template <CodeUnit CharT>
constexpr auto checked_decode_step(std::span<const CharT> input, size_t offset) -> DecodedRune {
    const auto result = decode_step(input);
    if (result.error != UnicodeDecodeError::None) [[unlikely]] {
        throw_decode_error<CharT>(result.error, offset);
    }
    assert_check([&] { return result.code_units > 0 && result.code_units <= input.size(); },
                 "Unicode decoder produced invalid progress");
    return result;
}

// Typed storage for one encoded scalar; no cross-character pointer casts are needed.
template <CharType CharT>
struct EncodedRune {
    static constexpr auto capacity = encoding_of_v<CharT> == Encoding::UTF8    ? size_t{4}
                                     : encoding_of_v<CharT> == Encoding::UTF16 ? size_t{2}
                                                                               : size_t{1};
    std::array<CharT, capacity> units{};
    uint8_t size{};
};

// encode_one_utf8 encodes a single Rune into a UTF-8 sequence.
template <CharType CharT>
    requires(encoding_of_v<CharT> == Encoding::UTF8)
constexpr auto encode_one_utf8(Rune rune) noexcept -> EncodedRune<CharT> {
    if (rune < 0x80) {
        return {{static_cast<CharT>(rune), 0, 0, 0}, 1};
    }
    if (rune < 0x800) {
        return {{static_cast<CharT>(0xC0 | (rune >> 6)), static_cast<CharT>(0x80 | (rune & 0x3F)), 0, 0}, 2};
    }
    if (rune < 0x10000) {
        return {{static_cast<CharT>(0xE0 | (rune >> 12)), static_cast<CharT>(0x80 | ((rune >> 6) & 0x3F)),
                 static_cast<CharT>(0x80 | (rune & 0x3F)), 0},
                3};
    }
    // Invalid code points are rejected before this encoding kernel is called.
    return {{static_cast<CharT>(0xF0 | (rune >> 18)), static_cast<CharT>(0x80 | ((rune >> 12) & 0x3F)),
             static_cast<CharT>(0x80 | ((rune >> 6) & 0x3F)), static_cast<CharT>(0x80 | (rune & 0x3F))},
            4};
}

// encode_one_utf16 encodes a single Rune into a UTF-16 sequence.
template <CharType CharT>
    requires(encoding_of_v<CharT> == Encoding::UTF16)
constexpr auto encode_one_utf16(Rune rune) noexcept -> EncodedRune<CharT> {
    if (rune < 0x10000) {
        return {{static_cast<CharT>(rune), 0}, 1};
    }
    // Invalid code points are rejected before this encoding kernel is called.
    const auto adjusted = rune - 0x10000;
    return {{static_cast<CharT>(0xD800 + (adjusted >> 10)), static_cast<CharT>(0xDC00 + (adjusted & 0x3FF))}, 2};
}

// encode_one_utf32 encodes a single Rune into a UTF-32 code unit (identity).
template <CharType CharT>
    requires(encoding_of_v<CharT> == Encoding::UTF32)
constexpr auto encode_one_utf32(Rune rune) noexcept -> EncodedRune<CharT> {
    // Invalid code points are rejected before this encoding kernel is called.
    return {{static_cast<CharT>(rune)}, 1};
}

// Encode a validated scalar using storage of the requested output character type.
template <CharType CharT>
constexpr auto encode_step(Rune rune) noexcept -> EncodedRune<CharT> {
    assert_check([=] { return is_unicode_scalar(rune); }, "Invalid internal Unicode scalar");
    if constexpr (encoding_of_v<CharT> == Encoding::UTF8) {
        return encode_one_utf8<CharT>(rune);
    } else if constexpr (encoding_of_v<CharT> == Encoding::UTF16) {
        return encode_one_utf16<CharT>(rune);
    } else {
        return encode_one_utf32<CharT>(rune);
    }
}

// Public encoding accepts user-provided Rune values and reports their position on failure.
template <CharType CharT>
constexpr auto checked_encode_step(Rune rune, size_t index) -> EncodedRune<CharT> {
    if (!is_unicode_scalar(rune)) [[unlikely]] {
        check(false, "Invalid Unicode scalar U+{:X} at rune index {}", static_cast<uint32_t>(rune), index);
    }
    return encode_step<CharT>(rune);
}

// Copy source code units after range and offset validity has been established by the caller.
// Generated offsets and segmentation ranges are internal invariants; keep their diagnostics in debug builds.
template <CharType CharT>
inline auto encode_validated_source(std::basic_string_view<CharT> source, std::span<const SourceOffset> offsets,
                                    WordRange range) -> std::basic_string<CharT> {
    assert_check([&] { return range.begin <= range.end && range.end < offsets.size(); },
                 "Invalid internal rune range [{}, {}) for {} Unicode offsets", range.begin, range.end, offsets.size());
    const auto begin = offsets[to_index(range.begin)];
    const auto end = offsets[to_index(range.end)];
    assert_check([&] { return begin <= end && end <= source.size(); },
                 "Invalid internal source offsets [{}, {}) for {} code units", begin, end, source.size());
    if (begin == end) {
        return {};
    }
    return std::basic_string<CharT>{source.data() + static_cast<size_t>(begin), static_cast<size_t>(end - begin)};
}

// Check representability before narrowing positions or allocating the offset sentinel.
constexpr auto offset_count_for_source(size_t source_size) noexcept -> size_t {
    assert_check(
        [&] {
            return size_fits_capacity<SourceOffset>(source_size) && source_size < std::numeric_limits<size_t>::max();
        },
        "Source length {} exceeds the configured Unicode offset range", source_size);
    return source_size + 1;
}

// Verify generated offsets once in debug builds, rather than rescanning them for every word.
inline auto valid_decoded_offsets(const UnicodeWithOffset &decoded, size_t source_size) noexcept -> bool {
    if (decoded.offsets.size() != decoded.runes.size() + 1 || decoded.offsets.empty() || decoded.offsets.front() != 0
        || decoded.offsets.back() != source_size) {
        return false;
    }
    return std::adjacent_find(decoded.offsets.begin(), decoded.offsets.end(),
                              [](SourceOffset left, SourceOffset right) { return left >= right; })
           == decoded.offsets.end();
}

// Compile out offset recording for callers that only need decoded runes.
enum class OffsetMode { Omit, Record };

// Decoding clears and may reallocate its output, so it cannot read from that same allocation.
template <CodeUnit CharT, typename Value>
auto overlaps_decode_buffer(std::span<const CharT> input, const std::vector<Value> &output) noexcept -> bool {
    if (input.empty() || output.empty()) {
        return false;
    }
    const auto less = std::less<const void *>{};
    return less(input.data(), output.data() + output.size()) && less(output.data(), input.data() + input.size());
}

// Share traversal and validation between both public decoding APIs.
template <OffsetMode Mode, CodeUnit CharT>
auto decode_into_impl(std::span<const CharT> input,
                      std::conditional_t<Mode == OffsetMode::Record, UnicodeWithOffset, Unicode> &result) -> void {
    auto &runes = [&]() -> Unicode & {
        if constexpr (Mode == OffsetMode::Record) {
            return result.runes;
        } else {
            return result;
        }
    }();
    assert_check([&] { return !overlaps_decode_buffer(input, runes); }, "Decoding input aliases its rune buffer");
    runes.clear();
    if constexpr (Mode == OffsetMode::Record) {
        assert_check([&] { return !overlaps_decode_buffer(input, result.offsets); },
                     "Decoding input aliases its offset buffer");
        result.offsets.clear();
        result.offsets.reserve(offset_count_for_source(input.size()));
    }
    runes.reserve(input.size()); // reserve enough space to avoid multiple allocations

    for (auto i = size_t{0}; i < input.size();) {
        // Invalid UTF-8/UTF-16 sequences throw instead of discarding the decoded prefix.
        const auto decoded = checked_decode_step(input.subspan(i), i);
        if constexpr (Mode == OffsetMode::Record) {
            result.offsets.push_back(static_cast<SourceOffset>(i));
        }
        runes.push_back(decoded.rune);
        i += decoded.code_units;
    }
    if constexpr (Mode == OffsetMode::Record) {
        result.offsets.push_back(static_cast<SourceOffset>(input.size()));
        assert_check([&] { return valid_decoded_offsets(result, input.size()); }, "Invalid generated Unicode offsets");
    }
}

// One-shot decoding shares the same validation as callers that reuse their buffers.
template <OffsetMode Mode, CodeUnit CharT>
auto decode_impl(std::span<const CharT> input)
    -> std::conditional_t<Mode == OffsetMode::Record, UnicodeWithOffset, Unicode> {
    auto result = std::conditional_t<Mode == OffsetMode::Record, UnicodeWithOffset, Unicode>{};
    decode_into_impl<Mode>(input, result);
    return result;
}

// Validate single-rune input before entering the decoding kernel.
template <CodeUnit CharT>
constexpr auto decode_one_impl(std::span<const CharT> input) -> Rune {
    if (input.empty()) [[unlikely]] {
        throw_decode_error<CharT>(UnicodeDecodeError::EmptyInput, 0);
    }
    return checked_decode_step(input, 0).rune;
}

// Materialize a checked scalar in the requested string encoding.
template <CharType CharT>
constexpr auto encode_one_impl(Rune rune) -> std::basic_string<CharT> {
    const auto encoded = checked_encode_step<CharT>(rune, 0);
    return std::basic_string<CharT>{encoded.units.data(), encoded.size};
}

// Encode validated runes using the shared scalar kernel and its debug assertions.
template <CharType CharT>
inline auto encode_validated_runes(std::span<const Rune> input) -> std::basic_string<CharT> {
    auto result = std::basic_string<CharT>{};
    result.reserve(input.size());
    for (const auto rune : input) {
        const auto encoded = encode_step<CharT>(rune);
        result.append(encoded.units.data(), encoded.size);
    }
    return result;
}

// Encode each rune while retaining its index for user-facing diagnostics.
template <CharType CharT>
inline auto encode_impl(std::span<const Rune> input) -> std::basic_string<CharT> {
    auto result = std::basic_string<CharT>{};
    result.reserve(input.size()); // at least one code unit per Rune

    for (auto i = size_t{0}; i < input.size(); ++i) {
        // Invalid code points are reported rather than converted to an empty string.
        const auto encoded = checked_encode_step<CharT>(input[i], i);
        result.append(encoded.units.data(), encoded.size);
    }
    return result;
}

// Validate caller-supplied ranges and offsets before copying source code units.
template <CharType CharT>
inline auto encode_source_impl(std::basic_string_view<CharT> source, std::span<const SourceOffset> offsets,
                               WordRange range) -> std::basic_string<CharT> {
    check(range.begin <= range.end && range.end < offsets.size(), "Invalid rune range [{}, {}) for {} Unicode offsets",
          range.begin, range.end, offsets.size());
    const auto begin = offsets[to_index(range.begin)];
    const auto end = offsets[to_index(range.end)];
    check(begin <= end && end <= source.size(), "Invalid source offsets [{}, {}) for {} code units", begin, end,
          source.size());
    return encode_validated_source(source, offsets, range);
}

// Unicode decoding always checks input bounds and rejects malformed sequences.
// These checks remain enabled in release builds because input may come from users or files.

// Validate the UTF encoding associated with the input's code-unit type without allocating decoding buffers.
// This checks well-formedness, not the original encoding of bytes that are valid in several encodings.
template <StringLike T>
[[nodiscard]] constexpr auto is_valid_utf(const T &input) -> bool {
    const auto units = as_code_units(input);
    for (auto offset = std::size_t{0}; offset < units.size();) {
        const auto decoded = decode_step(units.subspan(offset));
        if (decoded.error != UnicodeDecodeError::None) {
            return false;
        }
        offset += decoded.code_units;
    }
    return true;
}

// decode_one decodes a single Unicode code point from the input string and returns it as a Rune.
template <StringLike T>
constexpr auto decode_one(const T &input) -> Rune {
    return decode_one_impl(as_code_units(input));
}

// decode decodes an input string into a sequence of Unicode code points and returns it as a Unicode.
// T can be a std::string, std::string_view, char*, char[N], etc. — any StringLike type. (Former API.)
// Raw pointers now require an explicit sized view or buffer, as in every StringLike entry point.
template <StringLike T>
inline auto decode(const T &input) -> Unicode {
    return decode_impl<OffsetMode::Omit>(as_code_units(input));
}

// decode_with_offset decodes an input string into Unicode while recording source offsets
// for each code point, enabling fast re-encoding via UnicodeWithOffset.
template <StringLike T>
inline auto decode_with_offset(const T &input) -> UnicodeWithOffset {
    return decode_impl<OffsetMode::Record>(as_code_units(input));
}

// Replace runes and source offsets while retaining the destination buffers' capacity.
// Input must not alias either destination buffer. A failed decode leaves reusable, partial output.
template <StringLike T>
inline auto decode_with_offset_into(const T &input, UnicodeWithOffset &out) -> void {
    decode_into_impl<OffsetMode::Record>(as_code_units(input), out);
}

// encode_one encodes a single Rune into a string of the target CharT encoding.
template <CharType CharT = char>
constexpr auto encode_one(Rune rune) -> std::basic_string<CharT> {
    return encode_one_impl<CharT>(rune);
}

// encode encodes a sequence of Runes into a string of the target CharT encoding.
template <CharType CharT = char>
inline auto encode(std::span<const Rune> input) -> std::basic_string<CharT> {
    return encode_impl<CharT>(input);
}

// encodes a sequence of Unicode's WordRange [start, end) back to the source encoding using the provided offsets for
// fast lookup. it is faster than re-encoding each Rune when the target encoding matches the source encoding, as it can
// directly copy source code units. The source and offsets must come from the same validated input.
template <CharType CharT = char>
inline auto encode(std::basic_string_view<CharT> source, std::span<const SourceOffset> offsets, WordRange range)
    -> std::basic_string<CharT> {
    return encode_source_impl(source, offsets, range);
}

} // namespace detail

} // namespace neo_cppjieba
