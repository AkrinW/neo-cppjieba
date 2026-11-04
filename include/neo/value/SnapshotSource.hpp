#pragma once

#include "neo/Fwd.hpp"
#include "neo/util/FileIO.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace neo_cppjieba {

namespace detail {
template <typename Parse>
    requires std::invocable<Parse, std::span<const std::byte>, std::string_view>
auto with_snapshot_bytes(const SnapshotSource &source, Parse &&parse) -> decltype(auto);
} // namespace detail

// One dictionary snapshot: a file read completely into owned memory at load time, or bytes already in memory.
// Memory bytes are borrowed and must stay valid until the loader that reads them returns; loaded objects keep no
// reference to the bytes or the file. The path is copied; the name only labels diagnostics.
class SnapshotSource {
public:
    [[nodiscard]]
    static auto file(std::string_view path) -> SnapshotSource {
        return SnapshotSource{Kind::File, std::string{path}, {}};
    }

    [[nodiscard]]
    static auto memory(std::span<const std::byte> bytes, std::string_view name) -> SnapshotSource {
        return SnapshotSource{Kind::Memory, std::string{name}, bytes};
    }

    [[nodiscard]]
    auto is_file() const noexcept -> bool {
        return kind_ == Kind::File;
    }

    // The file path, or the diagnostic name of a memory source.
    [[nodiscard]]
    auto name() const noexcept -> std::string_view {
        return name_;
    }

private:
    // Where the bytes come from; a named value keeps the factory calls readable.
    enum class Kind : std::uint8_t { File, Memory };

    SnapshotSource(Kind kind, std::string name, std::span<const std::byte> bytes)
        : kind_(kind), name_(std::move(name)), bytes_(bytes) {
    }

    template <typename Parse>
        requires std::invocable<Parse, std::span<const std::byte>, std::string_view>
    friend auto detail::with_snapshot_bytes(const SnapshotSource &source, Parse &&parse) -> decltype(auto);

    Kind kind_;
    std::string name_;
    std::span<const std::byte> bytes_;
};

namespace detail {

// Loaders call parse(bytes, name). File contents live only for the duration of the call.
template <typename Parse>
    requires std::invocable<Parse, std::span<const std::byte>, std::string_view>
auto with_snapshot_bytes(const SnapshotSource &source, Parse &&parse) -> decltype(auto) {
    if (source.is_file()) {
        const auto file = read_file(source.name_);
        return std::forward<Parse>(parse)(std::as_bytes(std::span{file.content()}), source.name());
    }
    return std::forward<Parse>(parse)(source.bytes_, source.name());
}

} // namespace detail

} // namespace neo_cppjieba
