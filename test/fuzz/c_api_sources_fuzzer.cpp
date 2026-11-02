#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"
#include "neo/options/CutMode.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/SnapshotSource.hpp"
#include "neo/value/TaggedToken.hpp"
#include "neo/workspace/Workspace.hpp"

#include "CApiFuzzUtils.hpp"
#include "FuzzUtils.hpp"
#include "neo_cppjieba.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

// A request either loads a main dictionary and user dictionaries, or a dictionary snapshot.
enum class SourceKind : std::uint8_t { Dictionaries, Snapshot };

// One decoded request; texts borrow the fuzz input.
struct SourceRequest {
    SourceKind kind{};
    std::vector<std::string_view> texts;
};

// Text both engines cut: every valid UTF-8 source text, so their words are looked up, and a fixed sentence.
auto probe_text(std::span<const std::string_view> texts) -> std::string {
    auto probe = std::string{"我来到北京清华大学"};
    for (const auto text : texts) {
        if (Utf::is_valid(text)) {
            probe.append(text);
        }
    }
    return probe;
}

// Build the same engine through the C++ API; null when a source is rejected.
auto reference_engine(const SourceRequest &request) -> std::unique_ptr<Jieba> {
    const auto model = DictSource::memory(small_model);
    try {
        if (request.kind == SourceKind::Snapshot) {
            const auto bytes = std::as_bytes(std::span{request.texts.front()});
            return std::make_unique<Jieba>(Jieba::from_snapshot(SnapshotSource::memory(bytes, "<fuzz>"), model));
        }
        auto user_dicts = std::vector<DictSource>{};
        user_dicts.reserve(request.texts.size() - 1);
        for (const auto text : std::span{request.texts}.subspan(1)) {
            user_dicts.push_back(DictSource::memory(text));
        }
        return std::make_unique<Jieba>(DictSource::memory(request.texts.front()), model, user_dicts);
    } catch (const LogConfig::Exception &) {
        return nullptr;
    }
}

// Create the engine through the C ABI into out; corrupt descriptors must be rejected before any source is read.
auto create_engine(const SourceRequest &request, std::uint8_t corruption, ErrorProbe &error, EngineOwner &out)
    -> neo_jieba_status_v1 {
    auto sources = std::vector<neo_jieba_source_v1>{};
    sources.reserve(request.texts.size());
    std::ranges::transform(request.texts, std::back_inserter(sources), memory_source);
    if ((corruption & 1) != 0) {
        sources.front().kind = 0;
    }
    if ((corruption & 2) != 0) {
        sources.front().reserved = 1;
    }
    const auto model = memory_source(small_model);

    neo_jieba_engine_v1 *created = nullptr;
    const auto status =
        request.kind == SourceKind::Snapshot
            ? neo_jieba_v1_engine_create_from_snapshot(&sources.front(), &model, &created, error.buffer())
            : neo_jieba_v1_engine_create_from_sources(&sources.front(), &model, sources.data() + 1, sources.size() - 1,
                                                      &created, error.buffer());
    error.check(status);
    require((status == NEO_JIEBA_V1_OK) == (created != nullptr), "the engine slot disagrees with the status");
    out.reset(created);
    return status;
}

// Both engines tag and segment the probe identically, so the adapter passed every source through in order.
auto check_same_cut(const neo_jieba_engine_v1 *engine, const Jieba &reference, std::string_view probe) -> void {
    auto expected = std::vector<TaggedPosition>{};
    auto workspace = Workspace{};
    reference.cut_into(probe, CutMode::MIX, expected, workspace);

    const auto ranges = make_ranges();
    const auto c_workspace = make_workspace();
    require(neo_jieba_v1_cut_utf8_tagged_into(engine, probe.data(), probe.size(), NEO_JIEBA_V1_MIX, c_workspace.get(),
                                              ranges.get(), nullptr)
                == NEO_JIEBA_V1_OK,
            "a loaded engine did not cut the probe");
    auto data = neo_jieba_borrowed_ranges_v1{};
    require(neo_jieba_v1_ranges_borrow_data(ranges.get(), &data, nullptr) == NEO_JIEBA_V1_OK,
            "ranges_borrow_data failed");

    const auto actual = borrowed(data.ranges, data.count);
    const auto tags = borrowed(data.tags, data.count);
    require(actual.size() == expected.size(), "the C ABI and C++ engines cut a different number of words");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto &position = expected[index].position.source;
        require(actual[index].byte_offset == position.begin && actual[index].byte_length == position.size(),
                "the C ABI and C++ engines cut different words");
        require(std::ranges::equal(tags[index].data, std::span{expected[index].tag.data(), PosTag::MaxLength}),
                "the C ABI and C++ engines tag a word differently");
    }
}

} // namespace

// The first byte selects a snapshot load, corrupts the main descriptor's kind or reserved field, and picks the
// diagnostic capacity. A snapshot load reads the rest as the snapshot; otherwise up to four parts split at 0xFF
// bytes are the main dictionary and its user dictionaries. Every load uses the small HMM model.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    const auto flags = reader.byte();
    const auto kind = (flags & 1) != 0 ? SourceKind::Snapshot : SourceKind::Dictionaries;
    const auto rest = reader.rest();
    const auto request = SourceRequest{kind, kind == SourceKind::Snapshot ? std::vector{rest} : split_parts(rest, 4)};
    const auto corruption = static_cast<std::uint8_t>((flags >> 1) & 3);
    auto error = ErrorProbe{static_cast<std::uint8_t>(flags >> 3)};

    auto engine = EngineOwner{nullptr, neo_jieba_v1_engine_destroy};
    const auto status = create_engine(request, corruption, error, engine);
    if (corruption != 0) {
        require(status == NEO_JIEBA_V1_INVALID_ARGUMENT, "a corrupt source descriptor was accepted");
        return 0;
    }
    const auto reference = reference_engine(request);
    require((status == NEO_JIEBA_V1_OK) == (reference != nullptr), "the C ABI and C++ disagree on loading");
    if (reference == nullptr) {
        require(status == NEO_JIEBA_V1_OPERATION_FAILED, "a rejected source did not fail the operation");
        return 0;
    }
    check_same_cut(engine.get(), *reference, probe_text(request.texts));
    return 0;
}
