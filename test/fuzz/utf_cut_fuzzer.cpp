#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"
#include "neo/options/CutMode.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/TaggedToken.hpp"
#include "neo/workspace/Workspace.hpp"

#include "FuzzUtils.hpp"
#include "test_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

constexpr auto modes = std::array{CutMode::MIX,           CutMode::MIX_NO_HMM, CutMode::FULL, CutMode::SEARCH,
                                  CutMode::SEARCH_NO_HMM, CutMode::HMM,        CutMode::MP};

// Load the repository dictionaries once; libFuzzer runs every input in this process.
auto repository_jieba() -> const Jieba & {
    static const auto jieba = Jieba{DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8", ""};
    return jieba;
}

// The tagged positions of one cut in the input's own encoding.
template <CharType CharT>
auto tagged_cut(std::basic_string_view<CharT> input, CutMode mode) -> std::vector<TaggedPosition> {
    auto out = std::vector<TaggedPosition>{};
    auto workspace = Workspace{};
    repository_jieba().cut_into(input, mode, out, workspace);
    return out;
}

// Encodings agree on rune ranges and tags; only source offsets differ.
auto same_words(std::span<const TaggedPosition> left, std::span<const TaggedPosition> right) -> bool {
    return std::ranges::equal(left, right, [](const TaggedPosition &a, const TaggedPosition &b) {
        return a.position.runes == b.position.runes && a.tag == b.tag;
    });
}

// Each source range spells, in its own encoding, exactly the runes its rune range covers.
template <CharType CharT>
auto check_sources(std::span<const TaggedPosition> positions, std::basic_string_view<CharT> input,
                   std::span<const Rune> runes) -> void {
    for (const auto &item : positions) {
        const auto &[words, source] = item.position;
        require(source.begin <= source.end && source.end <= input.size(), "a source range exceeds the input");
        require(std::ranges::equal(Utf::decode(source.slice(input)), words.slice(runes)),
                "a source range spells other runes than its rune range");
    }
}

// Cutting invalid UTF-8 must throw the checked exception.
auto check_rejected(std::string_view utf8, CutMode mode) -> void {
    try {
        static_cast<void>(tagged_cut(utf8, mode));
    } catch (const LogConfig::Exception &) {
        return;
    }
    require(false, "invalid UTF-8 was not rejected");
}

} // namespace

// The first byte selects the mode; the rest is UTF-8 text, re-encoded as UTF-16 and UTF-32 when valid.
// All three encodings must segment and tag alike, with source offsets in their own code units.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    if (size == 0) {
        return 0;
    }
    const auto mode = modes[data[0] % modes.size()];
    const auto utf8 = as_text(data + 1, size - 1);
    if (!Utf::is_valid(utf8)) {
        check_rejected(utf8, mode);
        return 0;
    }
    const auto runes = Utf::decode(utf8);
    const auto utf16 = Utf::encode<char16_t>(runes);
    const auto utf32 = Utf::encode<char32_t>(runes);

    const auto from8 = tagged_cut(utf8, mode);
    const auto from16 = tagged_cut(std::u16string_view{utf16}, mode);
    const auto from32 = tagged_cut(std::u32string_view{utf32}, mode);
    require(same_words(from8, from16) && same_words(from8, from32), "encodings segment or tag differently");
    check_sources(from8, utf8, runes);
    check_sources(from16, std::u16string_view{utf16}, runes);
    check_sources(from32, std::u32string_view{utf32}, runes);
    return 0;
}
