#pragma once

#include "neo/Fwd.hpp"
#include "neo/workspace/KeywordState.hpp"

namespace neo_cppjieba {

namespace detail {
auto keyword_state(KeywordWorkspace &workspace) noexcept -> KeywordState &;
} // namespace detail

// Caller-owned storage for keyword extraction; reusable across inputs and both extractors.
// One active call owns the workspace; results never borrow its buffers.
class KeywordWorkspace {
public:
    KeywordWorkspace() = default;
    KeywordWorkspace(KeywordWorkspace &&) noexcept = default;
    auto operator=(KeywordWorkspace &&) noexcept -> KeywordWorkspace & = default;
    KeywordWorkspace(const KeywordWorkspace &) = delete;
    auto operator=(const KeywordWorkspace &) -> KeywordWorkspace & = delete;

    auto release() noexcept -> void {
        *this = KeywordWorkspace{};
    }

private:
    friend
    auto detail::keyword_state(KeywordWorkspace &workspace) noexcept -> detail::KeywordState &;

    detail::KeywordState state_;
};

namespace detail {

// The single access point to a keyword workspace's buffers; extractors unpack it before calling algorithms.
inline
auto keyword_state(KeywordWorkspace &workspace) noexcept -> KeywordState & {
    return workspace.state_;
}

} // namespace detail

} // namespace neo_cppjieba
