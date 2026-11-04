// Instantiates the per-rune segmentation paths for CheckInlining.cmake. InlineTests.cmake builds it with GCC's inline
// budget exhausted, so a hot helper left out of line here would be out of line in any large translation unit.
// The program is inspected with nm and never run.
#include "neo/Jieba.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

auto main(int argc, char *argv[]) -> int {
    const auto args = std::span{argv, static_cast<std::size_t>(argc)};
    if (args.size() != 4) {
        return 2;
    }
    const auto jieba = neo_cppjieba::Jieba{args[1], args[2], ""};
    auto workspace = neo_cppjieba::Workspace{};
    auto positions = std::vector<neo_cppjieba::TokenPosition>{};
    jieba.cut_into(std::string_view{args[3]}, neo_cppjieba::CutMode::MIX, positions, workspace);
    return positions.empty() ? 1 : 0;
}
