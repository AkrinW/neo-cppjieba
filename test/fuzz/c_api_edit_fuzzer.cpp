#include "neo/Utf.hpp"

#include "CApiFuzzUtils.hpp"
#include "FuzzUtils.hpp"
#include "neo_cppjieba.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

// The edits a harness applies to its clone of the base engine.
enum class EditKind : std::uint8_t { AddWord, AddWordWithFreq, RemoveWord, Compact };

// One decoded edit; word borrows the fuzz input.
struct Edit {
    EditKind kind{};
    std::string_view word;
    neo_jieba_pos_tag_v1 tag{};
    std::int32_t freq = 0;
};

// Empty, short and four-byte tags, and one whose padding is not all NUL.
constexpr auto tags = std::array{neo_jieba_pos_tag_v1{}, neo_jieba_pos_tag_v1{{'n'}}, neo_jieba_pos_tag_v1{{'n', 'z'}},
                                 neo_jieba_pos_tag_v1{{'a', 'b', 'c', 'd'}}, neo_jieba_pos_tag_v1{{'n', '\0', 'x'}}};

// The base engine loads the small sources once; each input edits its own clone and never the base.
auto base_engine() -> const neo_jieba_engine_v1 * {
    static const auto *const owned = [] {
        const auto dict = memory_source(small_dict);
        const auto model = memory_source(small_model);
        neo_jieba_engine_v1 *created = nullptr;
        require(neo_jieba_v1_engine_create_from_sources(&dict, &model, nullptr, 0, &created, nullptr)
                    == NEO_JIEBA_V1_OK,
                "engine_create_from_sources failed");
        return created;
    }();
    return owned;
}

// Each edit takes a kind byte, a length byte and that many word bytes, a tag byte and a frequency byte.
auto decode_edits(ByteReader &reader) -> std::vector<Edit> {
    auto edits = std::vector<Edit>{};
    while (!reader.empty()) {
        auto edit = Edit{};
        edit.kind = static_cast<EditKind>(reader.byte() % 4);
        edit.word = reader.text(reader.byte() % 16);
        edit.tag = tags[reader.byte() % tags.size()];
        edit.freq = static_cast<std::int32_t>(reader.byte()) - 16;
        edits.push_back(edit);
    }
    return edits;
}

// The probe holds every valid edited word, so each cut looks them up, after a fixed sentence.
auto probe_text(std::span<const Edit> edits) -> std::string {
    auto probe = std::string{"我来到北京清华大学"};
    for (const auto &edit : edits) {
        if (Utf::is_valid(edit.word)) {
            probe.append(edit.word);
        }
    }
    return probe;
}

// Argument errors come before decoding errors, matching the order the adapter checks them in.
auto expected_status(const Edit &edit) -> neo_jieba_status_v1 {
    if (edit.kind == EditKind::Compact) {
        return NEO_JIEBA_V1_OK;
    }
    const auto adds = edit.kind != EditKind::RemoveWord;
    if (edit.word.empty() || (adds && !is_padded_tag(edit.tag))
        || (edit.kind == EditKind::AddWordWithFreq && edit.freq <= 0)) {
        return NEO_JIEBA_V1_INVALID_ARGUMENT;
    }
    return Utf::is_valid(edit.word) ? NEO_JIEBA_V1_OK : NEO_JIEBA_V1_OPERATION_FAILED;
}

// Apply one edit; removed receives the removal flag of a RemoveWord edit.
auto apply(neo_jieba_engine_v1 *engine, const Edit &edit, std::uint32_t &removed, ErrorProbe &error)
    -> neo_jieba_status_v1 {
    switch (edit.kind) {
        case EditKind::AddWord: {
            return neo_jieba_v1_engine_add_word(engine, edit.word.data(), edit.word.size(), &edit.tag, error.buffer());
        }
        case EditKind::AddWordWithFreq: {
            return neo_jieba_v1_engine_add_word_with_freq(engine, edit.word.data(), edit.word.size(), edit.freq,
                                                          &edit.tag, error.buffer());
        }
        case EditKind::RemoveWord: {
            return neo_jieba_v1_engine_remove_word(engine, edit.word.data(), edit.word.size(), &removed,
                                                   error.buffer());
        }
        case EditKind::Compact: {
            return neo_jieba_v1_engine_compact(engine, error.buffer());
        }
    }
    require(false, "unknown edit kind");
    return NEO_JIEBA_V1_INVALID_ARGUMENT;
}

// Cuts compare by every range.
auto same_cut(std::span<const neo_jieba_token_range_v1> left, std::span<const neo_jieba_token_range_v1> right) -> bool {
    return std::ranges::equal(left, right, same_range);
}

// A word added with a nonempty tag reports that tag until a later edit changes it.
auto check_added_tag(const neo_jieba_engine_v1 *engine, const Edit &edit) -> void {
    if (edit.tag.data[0] == '\0') {
        return;
    }
    auto tag = neo_jieba_pos_tag_v1{};
    require(neo_jieba_v1_lookup_tag(engine, edit.word.data(), edit.word.size(), &tag, nullptr) == NEO_JIEBA_V1_OK,
            "lookup_tag rejected an added word");
    require(same_tag(tag, edit.tag), "an added word does not report its tag");
}

} // namespace

// The first byte picks the diagnostic capacity; the rest is a sequence of edits applied to a clone of the base.
// Rejected edits and compact leave segmentation unchanged, removal reports words this sequence added or removed,
// and the base engine cuts as before however its clone was edited.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    auto error = ErrorProbe{reader.byte()};
    const auto edits = decode_edits(reader);
    const auto probe = probe_text(edits);
    const auto base_cut = owned_ranges(base_engine(), probe, NEO_JIEBA_V1_MIX);

    neo_jieba_engine_v1 *created = nullptr;
    require(neo_jieba_v1_engine_clone(base_engine(), &created, nullptr) == NEO_JIEBA_V1_OK, "engine_clone failed");
    const auto clone = EngineOwner{created, neo_jieba_v1_engine_destroy};
    auto cut = owned_ranges(clone.get(), probe, NEO_JIEBA_V1_MIX);
    require(same_cut(cut, base_cut), "a clone cuts differently from its source");

    // Present words map to true and removed words to false; dictionary words not yet edited are unknown.
    auto present = std::map<std::string_view, bool>{};
    for (const auto &edit : edits) {
        auto removed = std::uint32_t{2};
        const auto status = apply(clone.get(), edit, removed, error);
        error.check(status);
        require(status == expected_status(edit), "an edit returned an unexpected status");
        require(edit.kind != EditKind::RemoveWord || removed <= 1, "remove_word left its flag unset");
        if (status != NEO_JIEBA_V1_OK) {
            require(edit.kind != EditKind::RemoveWord || removed == 0, "a failed removal left its flag set");
            require(same_cut(owned_ranges(clone.get(), probe, NEO_JIEBA_V1_MIX), cut),
                    "a rejected edit changed segmentation");
            continue;
        }
        if (edit.kind == EditKind::RemoveWord) {
            const auto known = present.find(edit.word);
            require(known == present.end() || removed == (known->second ? 1U : 0U),
                    "remove_word disagrees with earlier edits");
            present[edit.word] = false;
        } else if (edit.kind != EditKind::Compact) {
            check_added_tag(clone.get(), edit);
            present[edit.word] = true;
        }
        const auto next = owned_ranges(clone.get(), probe, NEO_JIEBA_V1_MIX);
        require(edit.kind != EditKind::Compact || same_cut(next, cut), "compact changed segmentation");
        cut = next;
    }
    require(same_cut(owned_ranges(base_engine(), probe, NEO_JIEBA_V1_MIX), base_cut),
            "edits on a clone changed its source engine");
    return 0;
}
