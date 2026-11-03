#pragma once

#include "neo/Config.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>
#include <unicode/umachine.h>
#include <unicode/utypes.h>

namespace neo_cppjieba {

// Byte encodings whose decoded rune boundaries support independent source slices.
enum class EncodingId : uint8_t { UTF8, GBK, GB18030 };

// Owns encoded bytes and their rune boundaries; offsets.back() equals bytes.size(), including for empty input.
struct EncodedTextWithOffset {
    std::string bytes;
    std::vector<SourceOffset> offsets;
};

namespace detail {

// Only these stateless, single-scalar mappings satisfy the original-byte slicing contract.
[[nodiscard]]
inline auto icu_encoding_name(EncodingId encoding) -> const char * {
    switch (encoding) {
        case EncodingId::UTF8:
            return "UTF-8";
        case EncodingId::GBK:
            return "GBK";
        case EncodingId::GB18030:
            return "GB18030";
    }
    assert_check([] { return false; }, "Unknown source encoding {}", std::to_underlying(encoding));
    std::unreachable();
}

// Releases the per-call converter even when malformed external text raises an exception.
using IcuConverter = std::unique_ptr<UConverter, decltype(&ucnv_close)>;

// ICU substitutes malformed input by default; install strict error reporting before decoding.
// Encoding also stops on unmappable scalars and disables optional fallback mappings.
[[nodiscard]]
inline auto open_icu_converter(const char *name) -> IcuConverter {
    auto error = U_ZERO_ERROR;
    auto converter = IcuConverter{ucnv_open(name, &error), &ucnv_close};
    check(U_SUCCESS(error) && converter, "Cannot open {} converter: {}", name, u_errorName(error));
    ucnv_setToUCallBack(converter.get(), UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &error);
    check(U_SUCCESS(error), "Cannot configure strict {} decoding: {}", name, u_errorName(error));
    ucnv_setFromUCallBack(converter.get(), UCNV_FROM_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &error);
    check(U_SUCCESS(error), "Cannot configure strict {} encoding: {}", name, u_errorName(error));
    ucnv_setFallback(converter.get(), false);
    return converter;
}

// Decode complete buffers while retaining capacity; omit source offsets when only UTF-8 output is needed.
template <OffsetMode Mode>
auto decode_icu_source_into(std::string_view input, EncodingId encoding,
                            std::conditional_t<Mode == OffsetMode::Record, UnicodeWithOffset, Unicode> &out) -> void {
    const auto *name = icu_encoding_name(encoding);
    if (encoding == EncodingId::UTF8) {
        decode_into_impl<Mode>(as_code_units(input), out);
        return;
    }

    auto &runes = [&]() -> Unicode & {
        if constexpr (Mode == OffsetMode::Record) {
            return out.runes;
        } else {
            return out;
        }
    }();
    assert(!overlaps_decode_buffer(as_code_units(input), runes));
    runes.clear();
    if constexpr (Mode == OffsetMode::Record) {
        assert(!overlaps_decode_buffer(as_code_units(input), out.offsets));
        out.offsets.clear();
        out.offsets.reserve(offset_count_for_source(input.size()));
    }
    runes.reserve(input.size());

    if (!input.empty()) {
        const auto converter = open_icu_converter(name);
        const auto *cursor = input.data();
        const auto *limit = cursor + input.size();
        while (cursor != limit) {
            const auto *start = cursor;
            const auto offset = static_cast<size_t>(start - input.data());
            auto error = U_ZERO_ERROR;
            const auto rune = ucnv_getNextUChar(converter.get(), &cursor, limit, &error);
            check(U_SUCCESS(error), "{} decoding failed at byte offset {}: {}", name, offset, u_errorName(error));
            assert(cursor > start && cursor <= limit);
            assert(is_unicode_scalar(static_cast<Rune>(rune)));
            if constexpr (Mode == OffsetMode::Record) {
                out.offsets.push_back(static_cast<SourceOffset>(offset));
            }
            runes.push_back(static_cast<Rune>(rune));
        }
    }
    if constexpr (Mode == OffsetMode::Record) {
        out.offsets.push_back(static_cast<SourceOffset>(input.size()));
        assert(valid_decoded_offsets(out, input.size()));
    }
}

// Encode one scalar with typed ICU storage; the supported stateless targets use at most four bytes per scalar.
inline auto append_icu_encoded_rune(UConverter *converter, const char *name, Rune rune, size_t utf8_offset,
                                    std::string &out) -> void {
    assert(converter != nullptr);
    const auto utf16 = encode_step<char16_t>(rune);
    const auto source = std::array<UChar, 2>{static_cast<UChar>(utf16.units[0]), static_cast<UChar>(utf16.units[1])};
    auto bytes = std::array<char, 4>{};
    auto error = U_ZERO_ERROR;
    const auto written =
        ucnv_fromUChars(converter, bytes.data(), static_cast<int32_t>(bytes.size()), source.data(), utf16.size, &error);
    assert(error != U_BUFFER_OVERFLOW_ERROR);
    check(U_SUCCESS(error), "{} encoding failed for U+{:X} at UTF-8 byte offset {}: {}", name,
          static_cast<uint32_t>(rune), utf8_offset, u_errorName(error));
    // ICU's STOP callback still skips unmappable default-ignorable scalars, such as U+200D, and reports success.
    check(written > 0, "{} encoding failed for U+{:X} at UTF-8 byte offset {}: unrepresentable scalar was skipped",
          name, static_cast<uint32_t>(rune), utf8_offset);
    assert(static_cast<size_t>(written) <= bytes.size());
    out.append(bytes.data(), static_cast<size_t>(written));
}

// Validate UTF-8 while encoding each scalar; plain conversion allocates no rune or offset buffers.
template <OffsetMode Mode>
[[nodiscard]]
auto encode_icu_target(std::string_view input, EncodingId encoding)
    -> std::conditional_t<Mode == OffsetMode::Record, EncodedTextWithOffset, std::string> {
    const auto *name = icu_encoding_name(encoding);
    auto result = std::conditional_t<Mode == OffsetMode::Record, EncodedTextWithOffset, std::string>{};
    auto &bytes = [&]() -> std::string & {
        if constexpr (Mode == OffsetMode::Record) {
            return result.bytes;
        } else {
            return result;
        }
    }();
    bytes.reserve(input.size());
    if constexpr (Mode == OffsetMode::Record) {
        result.offsets.reserve(offset_count_for_source(input.size()));
        result.offsets.push_back(0);
    }

    const auto converter =
        encoding == EncodingId::UTF8 || input.empty() ? IcuConverter{nullptr, &ucnv_close} : open_icu_converter(name);
    const auto units = as_code_units(input);
    for (auto offset = size_t{0}; offset < input.size();) {
        const auto decoded = checked_decode_step(units.subspan(offset), offset);
        if (encoding == EncodingId::UTF8) {
            bytes.append(input.data() + offset, decoded.code_units);
        } else {
            append_icu_encoded_rune(converter.get(), name, decoded.rune, offset, bytes);
        }
        if constexpr (Mode == OffsetMode::Record) {
            assert(bytes.size() <= std::numeric_limits<SourceOffset>::max());
            result.offsets.push_back(static_cast<SourceOffset>(bytes.size()));
        }
        offset += decoded.code_units;
    }
    return result;
}

} // namespace detail

// Optional ICU adapter. Converter state is local to each call; UTF-8 uses the existing native decoder.
class IcuCodec final {
public:
    IcuCodec() = delete;

    // Input must not alias either output buffer. Failure may leave a decoded prefix for subsequent reuse.
    static auto decode_with_offset_into(std::string_view input, EncodingId encoding, UnicodeWithOffset &out) -> void {
        detail::decode_icu_source_into<detail::OffsetMode::Record>(input, encoding, out);
    }

    [[nodiscard]]
    static auto to_utf8(std::string_view input, EncodingId encoding) -> std::string {
        auto runes = detail::Unicode{};
        detail::decode_icu_source_into<detail::OffsetMode::Omit>(input, encoding, runes);
        return detail::encode<char>(runes);
    }

    [[nodiscard]]
    static auto from_utf8(std::string_view input, EncodingId encoding) -> std::string {
        return detail::encode_icu_target<detail::OffsetMode::Omit>(input, encoding);
    }

    // offsets[i] locates rune boundary i in the returned bytes, not in the UTF-8 input.
    [[nodiscard]]
    static auto from_utf8_with_offset(std::string_view input, EncodingId encoding) -> EncodedTextWithOffset {
        return detail::encode_icu_target<detail::OffsetMode::Record>(input, encoding);
    }
};

} // namespace neo_cppjieba
