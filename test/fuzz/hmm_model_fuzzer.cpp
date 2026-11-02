#include "neo/Config.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/value/DictSource.hpp"

#include "FuzzUtils.hpp"

#include <cstddef>
#include <cstdint>

using namespace neo_cppjieba;

// Malformed HMM models must be rejected through the checked exception; internal assertions stay fatal.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    try {
        const auto model = detail::HMModel{DictSource::memory(as_text(data, size))};
        static_cast<void>(model);
    } catch (const LogConfig::Exception &) { // NOLINT(bugprone-empty-catch): rejection is the expected outcome.
    }
    return 0;
}
