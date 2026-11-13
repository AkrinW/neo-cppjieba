#pragma once

#include "neo/Fwd.hpp"
#include "neo/util/Logging.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace neo_cppjieba {

/// PosTag is a compact representation of a Part-of-Speech tag.
///
/// All known POS tags in the jieba dictionary are at most 4 ASCII characters.
/// Instead of storing them as `std::string` (32 bytes on most 64-bit platforms due to SSO),
/// we store up to 4 characters in a null-padded `char[4]` array (4 bytes), achieving an
/// 8x size reduction per DictUnit.
///
/// Layout (byte order matches memory order — no endianness dependency):
///   data_[0] = tag[0]  (first character, or '\0' if empty)
///   data_[1] = tag[1]
///   data_[2] = tag[2]
///   data_[3] = tag[3]
///
/// An empty tag is represented by all-zero bytes.
///
/// Because the characters are stored contiguously in memory, `to_string_view()` returns
/// a `std::string_view` that directly references the internal `data_` array — zero-copy,
/// no external buffer required. The view is valid as long as the PosTag object is alive.
///
/// Why not enum?
///   An enum would be more rigid — adding a new tag requires updating the enum definition and
///   maintaining string↔enum mapping tables. PosTag works with *any* tag string ≤ 4 chars,
///   making it forward-compatible with custom user dictionaries.
///
/// Comparisons use `raw()` which packs the 4 bytes into a `uint32_t` — a single CPU
/// instruction on all modern platforms.
///
class PosTag {
public:
    static constexpr auto MaxLength = size_t{4};

    /// Default-construct an empty tag (equivalent to "").
    explicit constexpr PosTag() noexcept : data_{} {
    }

    /// Construct from a string_view containing at most 4 characters and no NUL bytes.
    /// Invalid input throws; the valid path remains usable during constant evaluation.
    explicit constexpr PosTag(std::string_view sv) : data_{} {
        if (sv.size() > MaxLength) [[unlikely]] {
            check(false, "POS tag length {} exceeds the maximum of {} bytes", sv.size(), MaxLength);
        }
        if (sv.find('\0') != std::string_view::npos) [[unlikely]] {
            check(false, "POS tag must not contain NUL bytes");
        }
        for (auto i = size_t{0}; i < sv.size(); ++i) {
            data_[i] = sv[i];
        }
    }

    /// Construct from a C-string literal.
    explicit constexpr PosTag(const char *s) : PosTag(checked_c_string(s)) {
    }

    /// Check whether the tag is empty (i.e. "").
    [[nodiscard]] constexpr
    auto empty() const noexcept -> bool {
        return data_[0] == '\0';
    }

    /// Return the length of the tag (0–4).
    [[nodiscard]] constexpr
    auto size() const noexcept -> size_t {
        for (auto i = size_t{0}; i < MaxLength; ++i) {
            if (data_[i] == '\0') {
                return i;
            }
        }
        return MaxLength;
    }

    /// Return a pointer to the internal character data.
    /// The data is not necessarily null-terminated when size() == MaxLength.
    /// Borrowing from a temporary is disabled to avoid dangling pointers.
    [[nodiscard]] constexpr
    auto data() const & noexcept -> const char * {
        return data_;
    }
    [[nodiscard]] constexpr
    auto data() const && noexcept -> const char * = delete;

    /// Return a string_view directly referencing the internal storage (zero-copy).
    /// The returned view is valid as long as this PosTag object is alive.
    /// Borrowing from a temporary is disabled to avoid dangling views.
    [[nodiscard]] constexpr
    auto to_string_view() const & noexcept -> std::string_view {
        return std::string_view{data_, size()};
    }
    [[nodiscard]] constexpr
    auto to_string_view() const && noexcept -> std::string_view = delete;

    /// Convert to std::string (explicit, makes a copy).
    [[nodiscard]]
    auto to_string() const -> std::string {
        return std::string{data_, size()};
    }

    /// Comparison operators — use raw() for single-instruction comparison.
    constexpr
    auto operator==(const PosTag &other) const noexcept -> bool {
        return raw() == other.raw();
    }
    constexpr
    auto operator!=(const PosTag &other) const noexcept -> bool {
        return raw() != other.raw();
    }
    /// Ordering follows the packed value, not lexicographical tag order.
    constexpr
    auto operator<(const PosTag &other) const noexcept -> bool {
        return raw() < other.raw();
    }

    /// Compare directly with a string_view.
    constexpr
    auto operator==(std::string_view sv) const noexcept -> bool {
        return to_string_view() == sv;
    }
    constexpr
    auto operator!=(std::string_view sv) const noexcept -> bool {
        return !(*this == sv);
    }

    /// Access the raw packed value (for efficient comparison / switch on raw()).
    [[nodiscard]] constexpr
    auto raw() const noexcept -> uint32_t {
        auto v = uint32_t{0};
        for (auto i = size_t{0}; i < MaxLength; ++i) {
            v |= static_cast<uint32_t>(static_cast<unsigned char>(data_[i])) << (i * 8);
        }
        return v;
    }

private:
    // Validate the pointer before string_view scans for the terminating NUL.
    static constexpr
    auto checked_c_string(const char *s) -> std::string_view {
        if (s == nullptr) [[unlikely]] {
            check(false, "POS tag C-string must not be null");
        }
        return std::string_view{s};
    }

    alignas(uint32_t) char data_[MaxLength];
};

// Verify that PosTag is exactly 4 bytes and trivially copyable.
static_assert(sizeof(PosTag) == 4, "PosTag must be exactly 4 bytes");
static_assert(alignof(PosTag) == 4, "PosTag must be 4-byte aligned");
static_assert(std::is_trivially_copyable_v<PosTag>, "PosTag must be trivially copyable");

// ─────────────────────────────────────────────────────────────────────────────
// Predefined constants for all POS tags found in the jieba dictionary.
//
// Organized by the ICTCLAS / ICTPOS3.0 classification from 词性标记.md.
// These are constexpr, zero-cost, and usable in switch statements (on raw()).
// ─────────────────────────────────────────────────────────────────────────────
namespace pos {

// ── 实词 (Content words) ─────────────────────────────────────────────────────

// 名词 (Nouns)
inline constexpr auto n = PosTag{"n"};       // 名词
inline constexpr auto nr = PosTag{"nr"};     // 人名
inline constexpr auto nrfg = PosTag{"nrfg"}; // (jieba扩展) 人名
inline constexpr auto nrt = PosTag{"nrt"};   // (jieba扩展) 人名
inline constexpr auto ns = PosTag{"ns"};     // 地名
inline constexpr auto nt = PosTag{"nt"};     // 机构团体名
inline constexpr auto nz = PosTag{"nz"};     // 其它专名
inline constexpr auto ng = PosTag{"ng"};     // 名词性语素

// 时间词 (Time words)
inline constexpr auto t = PosTag{"t"};   // 时间词
inline constexpr auto tg = PosTag{"tg"}; // 时间词性语素

// 处所词 (Location words)
inline constexpr auto s = PosTag{"s"}; // 处所词

// 方位词 (Direction words)
inline constexpr auto f = PosTag{"f"}; // 方位词

// 动词 (Verbs)
inline constexpr auto v = PosTag{"v"};   // 动词
inline constexpr auto vd = PosTag{"vd"}; // 副动词
inline constexpr auto vn = PosTag{"vn"}; // 名动词
inline constexpr auto vg = PosTag{"vg"}; // 动词性语素
inline constexpr auto vi = PosTag{"vi"}; // 不及物动词
inline constexpr auto vq = PosTag{"vq"}; // (jieba扩展)

// 形容词 (Adjectives)
inline constexpr auto a = PosTag{"a"};   // 形容词
inline constexpr auto ad = PosTag{"ad"}; // 副形词
inline constexpr auto an = PosTag{"an"}; // 名形词
inline constexpr auto ag = PosTag{"ag"}; // 形容词性语素

// 区别词 (Distinguishing words)
inline constexpr auto b = PosTag{"b"}; // 区别词

// 状态词 (State words)
inline constexpr auto z = PosTag{"z"};   // 状态词
inline constexpr auto zg = PosTag{"zg"}; // (jieba扩展)

// 代词 (Pronouns)
inline constexpr auto r = PosTag{"r"};   // 代词
inline constexpr auto rr = PosTag{"rr"}; // 人称代词
inline constexpr auto rz = PosTag{"rz"}; // 指示代词
inline constexpr auto rg = PosTag{"rg"}; // 代词性语素

// 数词 (Numerals)
inline constexpr auto m = PosTag{"m"};   // 数词
inline constexpr auto mg = PosTag{"mg"}; // (jieba扩展)
inline constexpr auto mq = PosTag{"mq"}; // 数量词

// 量词 (Quantifiers)
inline constexpr auto q = PosTag{"q"}; // 量词

// ── 虚词 (Function words) ───────────────────────────────────────────────────

// 副词 (Adverbs)
inline constexpr auto d = PosTag{"d"};   // 副词
inline constexpr auto df = PosTag{"df"}; // (jieba扩展)
inline constexpr auto dg = PosTag{"dg"}; // 副词性语素

// 介词 (Prepositions)
inline constexpr auto p = PosTag{"p"}; // 介词

// 连词 (Conjunctions)
inline constexpr auto c = PosTag{"c"}; // 连词

// 助词 (Particles)
inline constexpr auto u = PosTag{"u"};   // 助词
inline constexpr auto ud = PosTag{"ud"}; // 的/底
inline constexpr auto ug = PosTag{"ug"}; // 过
inline constexpr auto uj = PosTag{"uj"}; // 的
inline constexpr auto ul = PosTag{"ul"}; // 了
inline constexpr auto uv = PosTag{"uv"}; // 地
inline constexpr auto uz = PosTag{"uz"}; // 着

// 叹词 (Interjections)
inline constexpr auto e = PosTag{"e"}; // 叹词

// 语气词 (Modal particles)
inline constexpr auto y = PosTag{"y"}; // 语气词

// 拟声词 (Onomatopoeia)
inline constexpr auto o = PosTag{"o"}; // 拟声词

// ── 其他 (Others) ───────────────────────────────────────────────────────────

inline constexpr auto g = PosTag{"g"}; // 语素
inline constexpr auto h = PosTag{"h"}; // 前缀
inline constexpr auto i = PosTag{"i"}; // 成语
inline constexpr auto j = PosTag{"j"}; // 简称略语
inline constexpr auto k = PosTag{"k"}; // 后缀
inline constexpr auto l = PosTag{"l"}; // 习用语
inline constexpr auto x = PosTag{"x"}; // 非语素字 / 字符串

// ── 特殊标记 (Special tags used by PosTagger) ───────────────────────────────

inline constexpr auto eng = PosTag{"eng"}; // 英文
inline constexpr auto unknown = PosTag{};  // 未知 (empty tag)

} // namespace pos
} // namespace neo_cppjieba
