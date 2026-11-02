#include "neo/Utf.hpp"

#include "CApiFuzzUtils.hpp"
#include "FuzzUtils.hpp"
#include "neo_cppjieba.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace neo_cppjieba;

namespace {

// Each tag is padded and is the tag lookup_tag reports for the word its range spells.
auto check_tags(const neo_jieba_borrowed_ranges_v1 &data, std::string_view input) -> void {
    const auto ranges = borrowed(data.ranges, data.count);
    const auto tags = borrowed(data.tags, data.count);
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        require(is_padded_tag(tags[index]), "a filled tag is not NUL-padded");
        const auto word = input.substr(ranges[index].byte_offset, ranges[index].byte_length);
        auto expected = neo_jieba_pos_tag_v1{};
        require(neo_jieba_v1_lookup_tag(repository_engine(), word.data(), word.size(), &expected, nullptr)
                    == NEO_JIEBA_V1_OK,
                "lookup_tag rejected a cut word");
        require(same_tag(tags[index], expected), "a filled tag differs from lookup_tag");
    }
}

// A fill reusing workspace and ranges matches an owned cut of the same input; any failure leaves ranges empty.
auto check_fill(std::string_view input, neo_jieba_mode_v1 mode, bool tagged, neo_jieba_workspace_v1 *workspace,
                neo_jieba_ranges_v1 *ranges, ErrorProbe &error) -> void {
    const auto fill = tagged ? &neo_jieba_v1_cut_utf8_tagged_into : &neo_jieba_v1_cut_utf8_into;
    const auto status = fill(repository_engine(), input.data(), input.size(), mode, workspace, ranges, error.buffer());
    error.check(status);

    auto data = neo_jieba_borrowed_ranges_v1{};
    require(neo_jieba_v1_ranges_borrow_data(ranges, &data, nullptr) == NEO_JIEBA_V1_OK, "ranges_borrow_data failed");
    if (!Utf::is_valid(input)) {
        require(status == NEO_JIEBA_V1_OPERATION_FAILED && data.count == 0 && data.ranges == nullptr
                    && data.tags == nullptr,
                "invalid UTF-8 was not rejected with empty ranges");
        return;
    }
    require(status == NEO_JIEBA_V1_OK, "valid UTF-8 was not cut");
    require(std::ranges::equal(borrowed(data.ranges, data.count), owned_ranges(repository_engine(), input, mode),
                               same_range),
            "ranges differ from the owned cut");
    if (!tagged) {
        require(data.tags == nullptr, "an untagged fill kept tags");
        return;
    }
    check_tags(data, input);
}

} // namespace

// The first byte selects the mode; the second tags the first and second fills and picks the diagnostic capacity.
// The rest holds both inputs, split at the first 0xFF byte, so the second fill reuses the first fill's storage.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    const auto mode = modes[reader.byte() % modes.size()];
    const auto flags = reader.byte();
    const auto inputs = split_parts(reader.rest(), 2);

    const auto ranges = make_ranges();
    const auto workspace = make_workspace();
    auto error = ErrorProbe{static_cast<std::uint8_t>(flags >> 2)};

    check_fill(inputs.front(), mode, (flags & 1) != 0, workspace.get(), ranges.get(), error);
    if (inputs.size() > 1) {
        check_fill(inputs.back(), mode, (flags & 2) != 0, workspace.get(), ranges.get(), error);
    }
    return 0;
}
