#include "neo/Utf.hpp"

#include "CApiFuzzUtils.hpp"
#include "FuzzUtils.hpp"
#include "neo_cppjieba.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

using namespace neo_cppjieba;

namespace {

// A result owns a copy of the input, and every range is a nonempty, well-formed UTF-8 slice of it.
auto check_tokens(const neo_jieba_tokens_v1 *tokens, std::string_view input, neo_jieba_mode_v1 mode) -> void {
    auto borrowed = neo_jieba_borrowed_tokens_v1{};
    require(neo_jieba_v1_tokens_borrow_data(tokens, &borrowed, nullptr) == NEO_JIEBA_V1_OK,
            "tokens_borrow_data failed");
    require(std::string_view{borrowed.utf8_data, static_cast<std::size_t>(borrowed.utf8_size)} == input,
            "result text differs from the input");

    const auto ranges = borrowed.count == 0 ? std::span<const neo_jieba_token_range_v1>{}
                                            : std::span{borrowed.ranges, static_cast<std::size_t>(borrowed.count)};
    auto end = std::uint64_t{0};
    for (const auto &range : ranges) {
        require(range.byte_length > 0, "empty token range");
        require(range.byte_offset <= input.size() && range.byte_length <= input.size() - range.byte_offset,
                "token range exceeds the input");
        require(Utf::is_valid(input.substr(range.byte_offset, range.byte_length)),
                "token range splits a UTF-8 sequence");
        require(!tiles_input(mode) || range.byte_offset == end, "tokens do not tile the input");
        end = range.byte_offset + range.byte_length;
    }
    require(!tiles_input(mode) || end == input.size(), "tokens do not cover the input");
}

} // namespace

// The first byte selects the cut mode; the remaining bytes are the UTF-8 input.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    if (size == 0) {
        return 0;
    }
    const auto mode = modes[data[0] % modes.size()];
    const auto input = as_text(data + 1, size - 1);

    neo_jieba_tokens_v1 *tokens = nullptr;
    const auto status =
        neo_jieba_v1_cut_utf8_owned(repository_engine(), input.data(), input.size(), mode, &tokens, nullptr);
    if (!Utf::is_valid(input)) {
        require(status == NEO_JIEBA_V1_OPERATION_FAILED && tokens == nullptr, "invalid UTF-8 was not rejected");
        return 0;
    }
    require(status == NEO_JIEBA_V1_OK, "valid UTF-8 was not cut");
    check_tokens(tokens, input, mode);
    neo_jieba_v1_tokens_destroy(tokens);
    return 0;
}
