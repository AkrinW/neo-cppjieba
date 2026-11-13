#include "neo/Config.hpp"
#include "neo/Utf.hpp"
#include "neo/util/Unicode.hpp"

#include "FuzzUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

// Copy fuzz bytes into whole code units; trailing bytes that do not fill a unit are ignored.
template <CharType CharT>
auto code_units(std::span<const std::uint8_t> bytes) -> std::basic_string<CharT> {
    auto units = std::basic_string<CharT>(bytes.size() / sizeof(CharT), CharT{});
    if (!units.empty()) {
        std::memcpy(units.data(), bytes.data(), units.size() * sizeof(CharT));
    }
    return units;
}

// Validation and both decoders must agree; valid input must round-trip and keep consistent source offsets.
template <CharType CharT>
auto check_decoding(std::basic_string_view<CharT> input) -> void {
    const auto valid = Utf::is_valid(input);
    auto runes = std::vector<Rune>{};
    try {
        runes = Utf::decode(input);
    } catch (const LogConfig::Exception &) {
        require(!valid, "decode rejected input that is_valid accepted");
        return;
    }
    require(valid, "decode accepted input that is_valid rejected");
    require(Utf::encode<CharT>(runes) == input, "valid input did not round-trip through encode");

    const auto decoded = detail::decode_with_offset(input);
    require(decoded.runes == runes, "decode_with_offset produced different runes");
    require(detail::valid_decoded_offsets(decoded, input.size()), "decode_with_offset produced invalid offsets");
}

} // namespace

extern
"C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const auto bytes = std::span{data, size};
    check_decoding(as_text(data, size));
    check_decoding(std::u16string_view{code_units<char16_t>(bytes)});
    check_decoding(std::u32string_view{code_units<char32_t>(bytes)});
    return 0;
}
