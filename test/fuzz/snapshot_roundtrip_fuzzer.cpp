#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"
#include "neo/options/CutMode.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/SnapshotSource.hpp"
#include "neo/value/TaggedToken.hpp"
#include "neo/workspace/Workspace.hpp"

#include "FuzzUtils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

constexpr auto modes = std::array{CutMode::MIX,           CutMode::MIX_NO_HMM, CutMode::FULL, CutMode::SEARCH,
                                  CutMode::SEARCH_NO_HMM, CutMode::HMM,        CutMode::MP};

// Text both engines cut: every valid UTF-8 source text, so each dictionary word is looked up, and a fixed sentence.
auto probe_text(std::string_view dict, std::span<const std::string_view> user_texts) -> std::string {
    auto probe = std::string{"我来到北京清华大学"};
    if (Utf::is_valid(dict)) {
        probe.append(dict);
    }
    for (const auto text : user_texts) {
        if (Utf::is_valid(text)) {
            probe.append(text);
        }
    }
    return probe;
}

// Load the engine from the dictionary sources; null when a source is rejected.
auto text_engine(const DictSource &dict, std::span<const DictSource> user_dicts) -> std::unique_ptr<Jieba> {
    try {
        return std::make_unique<Jieba>(dict, DictSource::memory(small_model), user_dicts);
    } catch (const LogConfig::Exception &) {
        return nullptr;
    }
}

// Encoding parses the same sources as loading, so it must reject what loading rejected.
auto check_encoding_rejected(const DictSource &dict, std::span<const DictSource> user_dicts) -> void {
    try {
        static_cast<void>(Jieba::encode_dict_snapshot(dict, user_dicts));
    } catch (const LogConfig::Exception &) {
        return;
    }
    require(false, "encoding accepted sources that loading rejected");
}

// The tagged positions of one cut.
auto tagged_cut(const Jieba &jieba, std::string_view probe, CutMode mode) -> std::vector<TaggedPosition> {
    auto out = std::vector<TaggedPosition>{};
    auto workspace = Workspace{};
    jieba.cut_into(probe, mode, out, workspace);
    return out;
}

} // namespace

// The first byte selects whether the first part is the main dictionary, otherwise the small dictionary is; the rest
// holds up to three parts split at 0xFF bytes, the remaining parts being user dictionaries. Sources that load must
// encode deterministically into a snapshot that cuts and tags exactly as the text-loaded engine in every mode.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    const auto fuzzed_main = (reader.byte() & 1) != 0;
    const auto parts = split_parts(reader.rest(), 3);
    const auto main_text = fuzzed_main ? parts.front() : small_dict;
    const auto user_texts = std::span{parts}.subspan(fuzzed_main ? 1 : 0);
    const auto dict = DictSource::memory(main_text);
    auto user_dicts = std::vector<DictSource>{};
    user_dicts.reserve(user_texts.size());
    for (const auto text : user_texts) {
        user_dicts.push_back(DictSource::memory(text));
    }

    const auto text = text_engine(dict, user_dicts);
    if (text == nullptr) {
        check_encoding_rejected(dict, user_dicts);
        return 0;
    }
    const auto bytes = Jieba::encode_dict_snapshot(dict, user_dicts);
    require(Jieba::encode_dict_snapshot(dict, user_dicts) == bytes, "encoding the same sources gave other bytes");
    const auto snapshot =
        Jieba::from_snapshot(SnapshotSource::memory(bytes, "<fuzz>"), DictSource::memory(small_model));
    const auto probe = probe_text(main_text, user_texts);
    for (const auto mode : modes) {
        require(tagged_cut(snapshot, probe, mode) == tagged_cut(*text, probe, mode),
                "the snapshot engine cuts differently from its sources");
    }
    return 0;
}
