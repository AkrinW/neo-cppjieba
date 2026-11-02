#include "neo/Config.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/value/DictSource.hpp"

#include "FuzzUtils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace neo_cppjieba;

namespace {

// A small valid main dictionary keeps each run focused on user-dictionary parsing and precedence.
constexpr auto main_dict = std::string_view{"北京 2000 ns\n清华大学 800 nt\n大学 1500 n\n来到 1200 v\n我 9000 r\n"};

} // namespace

// Malformed user dictionaries must be rejected through the checked exception; internal assertions stay fatal.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    const auto user_dicts = std::array{DictSource::memory(as_text(data, size))};
    try {
        const auto dict =
            detail::DictTrie{DictSource::memory(main_dict), user_dicts, detail::UserWordWeightOption::WordWeightMedian};
        static_cast<void>(dict);
    } catch (const LogConfig::Exception &) { // NOLINT(bugprone-empty-catch): rejection is the expected outcome.
    }
    return 0;
}
