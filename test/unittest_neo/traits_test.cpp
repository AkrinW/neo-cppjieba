#include "gtest/gtest.h"
#include "neo/concept/Text.hpp"
#include "neo/util/Unicode.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

namespace {

// A custom view conversion may only be used through the const input contract.
struct MutableViewSource {
    using value_type = char;
    operator std::string_view() {
        return "mutable";
    }
};

// Input adapters may report their own exceptions without terminating the process.
struct ThrowingViewSource {
    using value_type = char;
    operator std::string_view() const {
        throw std::logic_error{"view conversion failed"};
    }
};

// Character extraction must use the same const range access as as_view.
struct ConstOnlyRange {
    std::array<char, 2> characters{'a', 'b'};
    [[nodiscard]] auto begin() const -> const char * {
        return characters.data();
    }
    [[nodiscard]] auto end() const -> const char * {
        return characters.data() + characters.size();
    }
    auto begin() -> char * = delete;
    auto end() -> char * = delete;
};

// Test whether a value category may expose a borrowed view.
template <typename T>
concept CanView = requires(T &&input) { as_view(std::forward<T>(input)); };

// Typed code-unit views must enforce the same lifetime contract as string views.
template <typename T>
concept CanViewCodeUnits = requires(T &&input) { as_code_units(std::forward<T>(input)); };

// Pointer rejection must apply to every supported character encoding.
template <typename CharT>
class TraitsCharacterTest : public ::testing::Test {};

using TraitCharacterTypes = ::testing::Types<char, char8_t, char16_t, char32_t, wchar_t>;
TYPED_TEST_SUITE(TraitsCharacterTest, TraitCharacterTypes);

} // namespace

TEST(TraitsTest, OutputCharactersMustBeUnqualifiedCharacterTypes) {
    static_assert(CharType<char> && CharType<char8_t> && CharType<char16_t> && CharType<char32_t> && CharType<wchar_t>);
    static_assert(!CharType<const char> && !CharType<volatile char> && !CharType<char &>);
    static_assert(!CharType<unsigned char> && !CharType<std::byte> && !CharType<int>);
}

TEST(TraitsTest, EncodingLookupNormalizesCvAndReferenceQualifiers) {
    static_assert(encoding_of_v<const char &> == Encoding::UTF8);
    static_assert(encoding_of_v<char8_t> == Encoding::UTF8);
    static_assert(encoding_of_v<const char16_t> == Encoding::UTF16);
    static_assert(encoding_of_v<char32_t &&> == Encoding::UTF32);
    static_assert(encoding_of_v<wchar_t> == (sizeof(wchar_t) == 2 ? Encoding::UTF16 : Encoding::UTF32));
}

TEST(TraitsTest, StringLikeRequiresReadableConstCharacterStorage) {
    static_assert(StringLike<std::string> && StringLike<std::u8string> && StringLike<std::u16string>);
    static_assert(StringLike<std::u32string> && StringLike<std::wstring> && StringLike<std::string_view>);
    static_assert(StringLike<std::span<const char>> && StringLike<std::vector<char>>);
    static_assert(StringLike<std::array<char, 3>> && StringLike<char[3]>);
    static_assert(StringLike<ThrowingViewSource> && StringLike<ConstOnlyRange>);
    static_assert(!StringLike<MutableViewSource> && !StringLike<std::span<volatile char>>);
    static_assert(!StringLike<std::vector<int>> && !StringLike<int> && !StringLike<void>);
    static_assert(!StringLike<char[]> && !StringLike<std::nullptr_t>);
}

TEST(TraitsTest, BorrowedViewsRejectOwningTemporaries) {
    static_assert(CanView<std::string &> && CanView<const std::string &>);
    static_assert(CanView<std::vector<char> &> && CanView<std::array<char, 3> &>);
    static_assert(CanView<std::string_view> && CanView<std::span<const char>>);
    static_assert(!CanView<std::string> && !CanView<const std::string> && !CanView<std::vector<char>>);
    static_assert(!CanView<std::array<char, 3>>);
}

TEST(TraitsTest, FixedArrayDoesNotRequireTerminatingNul) {
    const char input[] = {'a', 'b', 'c'};
    EXPECT_EQ(as_view(input), "abc");
}

TEST(TraitsTest, LiteralPreservesEmbeddedNulAndOmitsTerminator) {
    constexpr auto view = as_view("a\0b");
    static_assert(view.size() == 3 && view[1] == '\0');
    EXPECT_EQ(view, (std::string_view{"a\0b", 3}));
}

TEST(TraitsTest, SizedRangeRetainsTrailingNul) {
    const auto input = std::array{'a', '\0', 'b', '\0'};
    const auto view = as_view(input);
    EXPECT_EQ(view.size(), input.size());
    EXPECT_EQ(view.data(), input.data());
    EXPECT_EQ(view.back(), '\0');
}

TEST(TraitsTest, EmptyRangesProduceEmptyViews) {
    const auto input = std::vector<char>{};
    EXPECT_TRUE(as_view(input).empty());
    EXPECT_TRUE(as_view(std::span<const char>{}).empty());
    EXPECT_TRUE(as_view("").empty());
}

TEST(TraitsTest, ConstOnlyRangeUsesConstAccess) {
    auto input = ConstOnlyRange{};
    EXPECT_EQ(as_view(input), "ab");
}

TEST(TraitsTest, ExplicitlySizedPointerViewsPreserveEmbeddedNul) {
    const char *input = "a\0b";
    const auto expected = std::string_view{"a\0b", 3};
    EXPECT_EQ(as_view(std::string_view{input, 3}), expected);
    EXPECT_EQ(as_view(std::span{input, std::size_t{3}}), expected);
}

TYPED_TEST(TraitsCharacterTest, RawPointersRequireExplicitLength) {
    static_assert(!StringLike<TypeParam *> && !StringLike<const TypeParam *>);
    static_assert(!StringLike<TypeParam *&> && !StringLike<const TypeParam *const &>);
    static_assert(!StringViewSource<TypeParam *> && !StringViewSource<const TypeParam *>);
    static_assert(!CanView<TypeParam *> && !CanView<const TypeParam *>);
    static_assert(!CanView<TypeParam *&> && !CanView<const TypeParam *const &>);
    static_assert(!CanViewCodeUnits<TypeParam *> && !CanViewCodeUnits<const TypeParam *>);
    static_assert(!CanViewCodeUnits<TypeParam *&> && !CanViewCodeUnits<const TypeParam *const &>);
}

TEST(TraitsTest, CustomConversionExceptionPropagates) {
    const auto input = ThrowingViewSource{};
    EXPECT_THROW(as_view(input), std::logic_error);
}

TEST(TraitsTest, ByteInputsRequireSizedContiguousStorage) {
    static_assert(StringLike<std::vector<std::byte>> && StringLike<std::pmr::vector<std::byte>>);
    static_assert(StringLike<std::array<unsigned char, 3>> && StringLike<signed char[3]>);
    static_assert(StringLike<std::span<const std::byte, 3>> && StringLike<std::span<unsigned char>>);
    static_assert(StringLike<std::vector<std::uint8_t>> && StringLike<std::vector<std::int8_t>>);
    static_assert(!StringLike<const std::byte *> && !StringLike<unsigned char *> && !StringLike<signed char *>);
    static_assert(!StringLike<std::byte[]> && !StringLike<std::span<volatile std::byte>>);
    static_assert(!StringLike<std::array<std::uint16_t, 3>> && !StringLike<std::array<std::uint32_t, 3>>);
    static_assert(!StringLike<std::array<bool, 3>>);
}

TEST(TraitsTest, ByteInputsUseUtf8AndProduceStandardCharStrings) {
    static_assert(encoding_of_v<const std::byte &> == Encoding::UTF8);
    static_assert(encoding_of_v<unsigned char> == Encoding::UTF8 && encoding_of_v<signed char> == Encoding::UTF8);
    static_assert(std::same_as<output_char_type_t<std::vector<std::byte>>, char>);
    static_assert(std::same_as<output_char_type_t<std::span<unsigned char>>, char>);
    static_assert(std::same_as<output_char_type_t<signed char[3]>, char>);
    static_assert(std::same_as<output_char_type_t<std::u8string>, char8_t>);
    static_assert(std::same_as<output_char_type_t<std::u16string>, char16_t>);
}

TEST(TraitsTest, ByteViewsRejectOwningTemporaries) {
    static_assert(CanView<std::vector<std::byte> &> && CanViewCodeUnits<std::vector<std::byte> &>);
    static_assert(CanView<std::span<const std::byte>> && CanViewCodeUnits<std::span<const std::byte>>);
    static_assert(!CanView<std::vector<std::byte>> && !CanViewCodeUnits<std::vector<std::byte>>);
    static_assert(!CanView<std::array<unsigned char, 3>> && !CanViewCodeUnits<std::array<unsigned char, 3>>);
}

TEST(TraitsTest, ByteArrayViewsRetainEmbeddedAndTrailingNul) {
    const std::byte input[] = {std::byte{'a'}, std::byte{}, std::byte{'b'}, std::byte{}};
    EXPECT_EQ(as_view(input), (std::string_view{"a\0b\0", 4}));
    EXPECT_EQ(as_code_units(input).data(), input);
    EXPECT_EQ(as_code_units(input).size(), 4U);
}

TEST(TraitsTest, ByteCodeUnitViewsSupportConstantEvaluation) {
    constexpr auto valid = [] {
        const std::byte input[] = {std::byte{0x41}, std::byte{}};
        const auto units = as_code_units(input);
        return units.size() == 2 && units[0] == std::byte{0x41} && units[1] == std::byte{};
    }();
    static_assert(valid);
    EXPECT_TRUE(valid);
}

TEST(TraitsTest, EmptyByteRangesProduceEmptyViews) {
    const auto input = std::vector<std::byte>{};
    EXPECT_TRUE(as_view(input).empty());
    EXPECT_TRUE(as_view(std::span<const unsigned char>{}).empty());
    EXPECT_TRUE(as_code_units(input).empty());
}
