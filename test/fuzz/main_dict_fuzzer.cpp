#include "neo/Config.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/value/DictSource.hpp"

#include "FuzzUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

using namespace neo_cppjieba;

// Malformed dictionaries must be rejected through the checked exception; internal assertions stay fatal.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    try {
        const auto dict = detail::DictTrie{DictSource::memory(as_text(data, size)), std::span<const DictSource>{},
                                           detail::UserWordWeightOption::WordWeightMedian};
        static_cast<void>(dict);
    } catch (const LogConfig::Exception &) { // NOLINT(bugprone-empty-catch): rejection is the expected outcome.
    }
    return 0;
}
