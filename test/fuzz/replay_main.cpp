#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <vector>

// Each harness defines the libFuzzer entry point; this driver calls it once per input.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int;

namespace {

// Expand directories into their regular files, sorted so a failure reproduces in the same order.
auto input_files(std::span<char *const> arguments) -> std::vector<std::filesystem::path> {
    auto files = std::vector<std::filesystem::path>{};
    for (const auto *argument : arguments) {
        const auto path = std::filesystem::path{argument};
        if (!std::filesystem::is_directory(path)) {
            files.push_back(path);
            continue;
        }
        const auto first = files.size();
        for (const auto &entry : std::filesystem::directory_iterator{path}) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(std::span{files}.subspan(first));
    }
    return files;
}

// Read one input as the raw bytes libFuzzer would pass; a missing or unreadable file fails the replay.
auto read_input(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes) -> bool {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    return !input.bad();
}

} // namespace

// Replay files and directories without libFuzzer, so every compiler and sanitizer build runs the corpus as a test.
auto main(int argc, char **argv) -> int {
    if (argc < 2) {
        std::fputs("usage: <replay> <input-file-or-directory>...\n", stderr);
        return 1;
    }
    const auto files = input_files(std::span{argv, static_cast<std::size_t>(argc)}.subspan(1));
    if (files.empty()) {
        std::fputs("no fuzz inputs to replay\n", stderr);
        return 1;
    }
    auto bytes = std::vector<std::uint8_t>{};
    for (const auto &file : files) {
        // Name the input first; stderr is unbuffered, so the name survives a harness abort.
        std::fprintf(stderr, "replaying %s\n", file.string().c_str());
        if (!read_input(file, bytes)) {
            std::fprintf(stderr, "cannot read %s\n", file.string().c_str());
            return 1;
        }
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
    std::printf("replayed %zu inputs\n", files.size());
    return 0;
}
