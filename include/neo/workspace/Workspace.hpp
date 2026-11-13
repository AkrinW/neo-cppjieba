#pragma once

#include "neo/Fwd.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include <vector>

namespace neo_cppjieba {

namespace detail {

// Per-call segmentation buffers behind the opaque Workspace: the decoded input, algorithm scratch and word ranges.
struct SegmentState {
    UnicodeWithOffset decoded;
    SegmentScratch scratch;
    std::vector<WordRange> ranges;
};

auto segment_state(Workspace &workspace) noexcept -> SegmentState &;

} // namespace detail

// Caller-owned decoding and segmentation storage. One active call owns the workspace;
// results never borrow its buffers, and capacity is retained until release or destruction.
class Workspace {
public:
    Workspace() = default;
    Workspace(Workspace &&) noexcept = default;
    auto operator=(Workspace &&) noexcept -> Workspace & = default;
    Workspace(const Workspace &) = delete;
    auto operator=(const Workspace &) -> Workspace & = delete;

    auto release() noexcept -> void {
        *this = Workspace{};
    }

private:
    friend
    auto detail::segment_state(Workspace &workspace) noexcept -> detail::SegmentState &;

    detail::SegmentState state_;
};

namespace detail {

// The single access point to a workspace's buffers; facades unpack it before calling algorithms.
inline
auto segment_state(Workspace &workspace) noexcept -> SegmentState & {
    return workspace.state_;
}

} // namespace detail

} // namespace neo_cppjieba
