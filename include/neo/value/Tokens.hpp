#pragma once

#include "neo/Fwd.hpp"
#include "neo/concept/Text.hpp"
#include "neo/options/Tagging.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/TaggedToken.hpp"
#include "neo/value/TokenView.hpp"

#include <concepts>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace neo_cppjieba {

namespace detail {

// Untagged and tagged results share storage and iteration; only the stored record and its view differ.
template <CharType CharT>
[[nodiscard]]
constexpr auto make_token_view(std::basic_string_view<CharT> source, const TokenPosition &position) noexcept
    -> TokenView<CharT> {
    return {position.source.slice(source), position};
}

template <CharType CharT>
[[nodiscard]]
constexpr auto make_token_view(std::basic_string_view<CharT> source, const TaggedPosition &tagged) noexcept
    -> TaggedTokenView<CharT> {
    return {tagged.position.source.slice(source), tagged.position, tagged.tag};
}

template <typename Position>
concept TokenRecord = std::same_as<Position, TokenPosition> || std::same_as<Position, TaggedPosition>;

// Iteration materializes views on demand instead of storing a text pointer in every token.
template <CharType CharT, TokenRecord Position = TokenPosition>
class TokenIterator {
public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;
    using value_type = decltype(make_token_view(std::basic_string_view<CharT>{}, std::declval<const Position &>()));
    using difference_type = std::ptrdiff_t;
    using reference = value_type;

    TokenIterator() noexcept = default;
    TokenIterator(std::basic_string_view<CharT> source,
                  typename std::vector<Position>::const_iterator position) noexcept
        : source_(source), position_(position) {
    }

    [[nodiscard]]
    auto operator*() const noexcept -> value_type {
        return make_token_view(source_, *position_);
    }

    auto operator++() noexcept -> TokenIterator & {
        ++position_;
        return *this;
    }

    auto operator++(int) noexcept -> TokenIterator {
        auto previous = *this;
        ++*this;
        return previous;
    }

    [[nodiscard]]
    auto operator==(const TokenIterator &other) const noexcept -> bool {
        return position_ == other.position_;
    }

private:
    std::basic_string_view<CharT> source_{};
    typename std::vector<Position>::const_iterator position_{};
};

// Results store a borrowed view of the input or the moved-in input string.
template <typename Storage>
concept TokenStorage = requires { typename Storage::value_type; } && CharType<typename Storage::value_type>
                       && (std::same_as<Storage, std::basic_string_view<typename Storage::value_type>>
                           || std::same_as<Storage, std::basic_string<typename Storage::value_type>>);

// Owns positions and stores either borrowed or owned source text; no cached pointers into owned strings.
// Tagged results store TaggedPosition records and iterate as TaggedTokenView.
template <TokenStorage Storage, TokenRecord Position = TokenPosition>
class BasicTokens {
public:
    using char_type = typename Storage::value_type;
    using position_type = Position;
    using const_iterator = TokenIterator<char_type, Position>;
    using value_type = typename const_iterator::value_type;

    [[nodiscard]]
    auto size() const noexcept -> size_t {
        return positions_.size();
    }

    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return positions_.empty();
    }

    [[nodiscard]]
    auto source() const & noexcept -> std::basic_string_view<char_type> {
        return source_;
    }
    [[nodiscard]]
    auto source() const && noexcept -> std::basic_string_view<char_type> = delete;

    [[nodiscard]]
    auto positions() const & noexcept -> std::span<const Position> {
        return positions_;
    }
    [[nodiscard]]
    auto positions() const && noexcept -> std::span<const Position> = delete;

    [[nodiscard]]
    auto operator[](size_t index) const & noexcept -> value_type {
        assert_check([&] { return index < positions_.size(); }, "Token index exceeds the result size");
        return make_token_view(source(), positions_[index]);
    }
    auto operator[](size_t) const && noexcept -> value_type = delete;

    [[nodiscard]]
    auto begin() const & noexcept -> const_iterator {
        return {source(), positions_.cbegin()};
    }
    [[nodiscard]]
    auto begin() const && noexcept -> const_iterator = delete;

    [[nodiscard]]
    auto end() const & noexcept -> const_iterator {
        return {source(), positions_.cend()};
    }
    [[nodiscard]]
    auto end() const && noexcept -> const_iterator = delete;

private:
    friend class ::neo_cppjieba::Jieba;

    BasicTokens(Storage source, std::vector<Position> positions) noexcept(std::is_nothrow_move_constructible_v<Storage>)
        : source_(std::move(source)), positions_(std::move(positions)) {
    }

    Storage source_;
    std::vector<Position> positions_;
};

} // namespace detail

// Owns token positions and borrows the input text, which must remain alive and unchanged while viewed.
// Tagging::Pos results iterate as TaggedTokenView instead of TokenView.
template <CharType CharT, Tagging tagging = Tagging::None>
using Tokens = detail::BasicTokens<std::basic_string_view<CharT>, detail::token_record_t<tagging>>;

// Owns both text and token positions; obtain fresh views after moving the result.
template <CharType CharT, Tagging tagging = Tagging::None>
using OwnedTokens = detail::BasicTokens<std::basic_string<CharT>, detail::token_record_t<tagging>>;

template <CharType CharT>
using TaggedTokens = Tokens<CharT, Tagging::Pos>;

template <CharType CharT>
using OwnedTaggedTokens = OwnedTokens<CharT, Tagging::Pos>;

} // namespace neo_cppjieba
