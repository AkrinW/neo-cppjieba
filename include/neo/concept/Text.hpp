#pragma once

#include <concepts>
#include <cstddef>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace neo_cppjieba {
static_assert(std::numeric_limits<unsigned char>::digits == 8, "Unicode byte input requires 8-bit bytes");

// ── Concepts ─────────────────────────────────────────────────
/// CharType: a character type whose encoding is known.
template <typename T>
concept CharType = std::same_as<T, char> || std::same_as<T, char8_t> || std::same_as<T, char16_t>
                   || std::same_as<T, char32_t> || std::same_as<T, wchar_t>;

namespace detail {

// Byte buffers carry UTF-8 octets, independent of signedness and native byte order.
template <typename T>
concept ByteType = std::same_as<T, std::byte> || std::same_as<T, unsigned char> || std::same_as<T, signed char>;

// Input code units also include byte storage; output strings retain standard character types.
template <typename T>
concept CodeUnit = CharType<T> || ByteType<T>;

// Use a consteval function and if constexpr to replace complex SFINAE template specializations,
// avoiding ambiguity during type extraction.
template <typename T>
consteval
auto get_char_type() noexcept {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_pointer_v<Decayed>) {
        // Pointer or decayed C-array (e.g., const char*, char[N])
        return std::type_identity<std::remove_cv_t<std::remove_pointer_t<Decayed>>>{};
    } else if constexpr (std::ranges::contiguous_range<const std::remove_reference_t<T>>) {
        // C++20 contiguous memory range (e.g., std::string, std::vector, std::span, std::array)
        return std::type_identity<std::remove_cv_t<std::ranges::range_value_t<const std::remove_reference_t<T>>>>{};
    } else if constexpr (requires { typename Decayed::value_type; }) {
        // Other non-standard custom containers with a nested value_type
        return std::type_identity<std::remove_cv_t<typename Decayed::value_type>>{};
    } else {
        return std::type_identity<void>{};
    }
}

// Extract the underlying character type
template <typename T>
using resolve_char_type_t = typename decltype(get_char_type<T>())::type;

// Require the same const access and readable code-unit pointer used by as_view.
template <typename T>
concept ConstCodeUnitRange =
    CodeUnit<resolve_char_type_t<T>> && std::ranges::contiguous_range<const std::remove_reference_t<T>>
    && std::ranges::sized_range<const std::remove_reference_t<T>> && requires(const std::remove_reference_t<T> &input) {
           { std::ranges::data(input) } -> std::convertible_to<const resolve_char_type_t<T> *>;
           { std::ranges::size(input) } -> std::convertible_to<std::size_t>;
       };

} // namespace detail

// Core StringLike Concept: any type convertible to a basic_string_view via as_view.
//  - Must have a valid CodeUnit type.
//  - Characters may convert to string_view; byte storage must be a contiguous range with a known size.
template <typename T>
concept StringLike =
    detail::CodeUnit<detail::resolve_char_type_t<T>> && !std::is_pointer_v<std::remove_cvref_t<T>>
    && (!std::is_array_v<std::remove_reference_t<T>> || std::is_bounded_array_v<std::remove_reference_t<T>>)
    && ((CharType<detail::resolve_char_type_t<T>>
         && std::convertible_to<const std::remove_reference_t<T> &,
                                std::basic_string_view<detail::resolve_char_type_t<T>>>)
        || detail::ConstCodeUnitRange<T>);

// Byte input produces UTF-8 std::string output; native character input preserves its character type.
template <StringLike T>
using output_char_type_t =
    std::conditional_t<detail::ByteType<detail::resolve_char_type_t<T>>, char, detail::resolve_char_type_t<T>>;

// Owning temporaries may be decoded, but must not expose a view that outlives them.
template <typename T>
concept StringViewSource = StringLike<T> && (std::is_lvalue_reference_v<T> || std::ranges::borrowed_range<T>);

namespace detail {

// Parses one loaded dictionary, model, IDF or stop-word text as parse(text, name); name labels diagnostics.
// The text is borrowed only for the duration of the call.
template <typename Parse>
concept SourceTextParser = std::invocable<Parse, std::string_view, std::string_view>;

} // namespace detail

} // namespace neo_cppjieba
