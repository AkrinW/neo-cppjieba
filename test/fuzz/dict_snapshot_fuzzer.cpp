#include "neo/Config.hpp"
#include "neo/resource/DictSnapshot.hpp"
#include "neo/util/ByteOrder.hpp"
#include "neo/util/Xxh64.hpp"
#include "neo/value/SnapshotSource.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace neo_cppjieba;

// Malformed snapshots must be rejected through the checked exception; internal assertions stay fatal.
// Mutations rarely keep a 64-bit checksum valid, so reseal it to reach the section and trie validators.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const auto input = std::as_bytes(std::span{data, size});
    auto bytes = std::vector<std::byte>{input.begin(), input.end()};
    if (bytes.size() >= detail::kSnapshotV1HeaderSize) {
        const auto header_size = detail::load_le<std::uint32_t>(std::span{bytes}.subspan(12));
        if (header_size >= detail::kSnapshotV1HeaderSize && header_size <= bytes.size()) {
            detail::store_le(std::span{bytes}.subspan(detail::kSnapshotV1ChecksumOffset),
                             detail::xxh64(std::span{bytes}.subspan(header_size), 0));
        }
    }
    try {
        const auto dict = detail::load_dict_snapshot(SnapshotSource::memory(bytes, "<fuzz>"));
        static_cast<void>(dict);
    } catch (const LogConfig::Exception &) { // NOLINT(bugprone-empty-catch): rejection is the expected outcome.
    }
    return 0;
}
