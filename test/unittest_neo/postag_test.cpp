#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/Jieba.hpp"

#include "test_paths.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace neo_cppjieba;

namespace {

// Check which value categories may borrow a tag's character storage.
template <typename T>
concept CanBorrowTagData = requires(T &&tag) { std::forward<T>(tag).data(); };

// Check which value categories may borrow a view of a tag.
template <typename T>
concept CanBorrowTagView = requires(T &&tag) { std::forward<T>(tag).to_string_view(); };

} // namespace

// ─── PosTag basic tests ─────────────────────────────────────────────────────

TEST(PosTagTest, DefaultIsEmpty) {
    auto tag = PosTag{};
    EXPECT_TRUE(tag.empty());
    EXPECT_EQ(tag.size(), 0u);
    EXPECT_EQ(tag.to_string(), "");
    EXPECT_EQ(tag.raw(), 0u);
}

TEST(PosTagTest, ConstructFromStringView) {
    const auto tag = PosTag{std::string_view{"nr"}};
    EXPECT_FALSE(tag.empty());
    EXPECT_EQ(tag.size(), 2u);
    EXPECT_EQ(tag.to_string(), "nr");
}

TEST(PosTagTest, ConstructMaxLength) {
    auto tag = PosTag{"nrfg"};
    EXPECT_EQ(tag.size(), 4u);
    EXPECT_EQ(tag.to_string(), "nrfg");
}

TEST(PosTagTest, RejectsStringViewLongerThanMaxLength) {
    EXPECT_THROW((void)PosTag{std::string_view{"abcde"}}, LogConfig::Exception);
}

TEST(PosTagTest, RejectsCStringLongerThanMaxLength) {
    EXPECT_THROW((void)PosTag{"abcde"}, LogConfig::Exception);
}

TEST(PosTagTest, RejectsNulBytesInStringView) {
    const auto embedded_nul = std::string_view{"n\0r", 3};
    const auto leading_nul = std::string_view{"\0n", 2};
    const auto trailing_nul = std::string_view{"n\0", 2};
    EXPECT_THROW((void)PosTag{embedded_nul}, LogConfig::Exception);
    EXPECT_THROW((void)PosTag{leading_nul}, LogConfig::Exception);
    EXPECT_THROW((void)PosTag{trailing_nul}, LogConfig::Exception);
}

TEST(PosTagTest, RejectsNullCString) {
    const char *input = nullptr;
    EXPECT_THROW((void)PosTag{input}, LogConfig::Exception);
}

TEST(PosTagTest, AcceptsEmptyStringView) {
    const auto tag = PosTag{std::string_view{}};
    EXPECT_TRUE(tag.empty());
    EXPECT_TRUE(tag.to_string_view().empty());
    EXPECT_EQ(tag, PosTag{});
}

TEST(PosTagTest, ValidConstructionRemainsConstantEvaluable) {
    constexpr auto tag = PosTag{std::string_view{"nrfg"}};
    constexpr auto c_string_tag = PosTag{"nr"};
    constexpr auto empty_tag = PosTag{std::string_view{}};
    static_assert(tag.size() == PosTag::MaxLength);
    static_assert(tag == std::string_view{"nrfg"});
    static_assert(c_string_tag == pos::nr);
    static_assert(empty_tag.empty());
    EXPECT_EQ(tag, pos::nrfg);
}

TEST(PosTagTest, InputConstructorsAllowExceptions) {
    static_assert(!std::is_nothrow_constructible_v<PosTag, std::string_view>);
    static_assert(!std::is_nothrow_constructible_v<PosTag, const char *>);
    static_assert(std::is_nothrow_default_constructible_v<PosTag>);
    static_assert(std::is_nothrow_copy_constructible_v<PosTag>);
    static_assert(std::is_nothrow_copy_assignable_v<PosTag>);
}

TEST(PosTagTest, Equality) {
    EXPECT_EQ(PosTag{"n"}, PosTag{"n"});
    EXPECT_NE(PosTag{"n"}, PosTag{"nr"});
    EXPECT_EQ(PosTag{}, PosTag{});
    EXPECT_NE(PosTag{}, PosTag{"x"});
}

// Ordering follows the packed value, so ordered containers key tags without comparing strings.
TEST(PosTagTest, OrderingFollowsThePackedValue) {
    const auto tags = std::array{PosTag{}, PosTag{"n"}, PosTag{"nr"}, PosTag{"v"}, PosTag{"abcd"}};
    for (const auto &left : tags) {
        EXPECT_FALSE(left < left);
        for (const auto &right : tags) {
            EXPECT_EQ(left < right, left.raw() < right.raw());
        }
    }
    EXPECT_EQ((std::set<PosTag>{PosTag{"n"}, PosTag{"v"}, PosTag{"n"}}.size()), 2U);
}

TEST(PosTagTest, CompareWithStringView) {
    auto tag = PosTag{"vn"};
    EXPECT_TRUE(tag == std::string_view("vn"));
    EXPECT_FALSE(tag == std::string_view("v"));
    EXPECT_TRUE(tag != std::string_view("ad"));
}

TEST(PosTagTest, LongerStringViewDoesNotCompareEqualToPrefix) {
    const auto tag = PosTag{"abcd"};
    const auto longer = std::string_view{"abcde"};
    EXPECT_FALSE(tag == longer);
    EXPECT_TRUE(tag != longer);
}

TEST(PosTagTest, StringViewWithNulDoesNotCompareEqualToTag) {
    const auto tag = PosTag{"n"};
    const auto padded = std::string_view{"n\0", 2};
    EXPECT_FALSE(tag == padded);
    EXPECT_TRUE(tag != padded);
}

TEST(PosTagTest, ExplicitStringConversion) {
    auto tag = PosTag{"ad"};
    auto s = tag.to_string();
    EXPECT_EQ(s, "ad");
}

TEST(PosTagTest, ToStringView) {
    auto tag = PosTag{"nrt"};
    auto sv = tag.to_string_view();
    EXPECT_EQ(sv, "nrt");
    // The view points directly into the PosTag's internal storage.
    EXPECT_EQ(sv.data(), tag.data());
}

TEST(PosTagTest, BorrowedStorageRequiresLvalue) {
    static_assert(CanBorrowTagData<PosTag &>);
    static_assert(CanBorrowTagData<const PosTag &>);
    static_assert(!CanBorrowTagData<PosTag>);
    static_assert(!CanBorrowTagData<const PosTag>);
    static_assert(CanBorrowTagView<PosTag &>);
    static_assert(CanBorrowTagView<const PosTag &>);
    static_assert(!CanBorrowTagView<PosTag>);
    static_assert(!CanBorrowTagView<const PosTag>);
}

TEST(PosTagTest, TemporaryCanProduceOwningString) {
    const auto text = PosTag{"nr"}.to_string();
    EXPECT_EQ(text, "nr");
}

TEST(PosTagTest, PredefinedConstants) {
    EXPECT_EQ(pos::n.to_string(), "n");
    EXPECT_EQ(pos::nrfg.to_string(), "nrfg");
    EXPECT_EQ(pos::eng.to_string(), "eng");
    EXPECT_TRUE(pos::unknown.empty());

    // Predefined constant matches runtime-constructed tag
    EXPECT_EQ(PosTag{"nrfg"}, pos::nrfg);
    EXPECT_EQ(PosTag{"eng"}, pos::eng);
}

TEST(PosTagTest, SizeAndAlignment) {
    EXPECT_EQ(sizeof(PosTag), 4u);
    EXPECT_EQ(alignof(PosTag), 4u);
    EXPECT_TRUE(std::is_trivially_copyable_v<PosTag>);
}

// ─── Read all tags from jieba.dict.utf8 ─────────────────────────────────────
namespace {
class PosTagDictTest : public ::testing::Test {
protected:
    // Parsed tag info from one line of the dictionary.
    struct DictEntry {
        std::string word;
        std::string freq;
        std::string tag_str;
    };

    inline static std::vector<DictEntry> entries_{};
    inline static bool loaded_{false};

    static auto SetUpTestSuite() -> void {
        if (loaded_) {
            return;
        }
        auto path = std::string(DICT_DIR) + "/jieba.dict.utf8";
        auto ifs = std::ifstream{path};
        ASSERT_TRUE(ifs.is_open()) << "Failed to open " << path;

        auto line = std::string{};
        while (std::getline(ifs, line)) {
            if (line.empty()) {
                continue;
            }
            // Each line: "word freq tag"
            auto p1 = line.find(' ');
            ASSERT_NE(p1, std::string::npos) << "Bad line: " << line;
            auto p2 = line.find(' ', p1 + 1);
            ASSERT_NE(p2, std::string::npos) << "Bad line: " << line;

            auto e = DictEntry{};
            e.word = line.substr(0, p1);
            e.freq = line.substr(p1 + 1, p2 - p1 - 1);
            e.tag_str = line.substr(p2 + 1);
            entries_.push_back(std::move(e));
        }
        loaded_ = true;
        ASSERT_FALSE(entries_.empty()) << "Dictionary is empty";
    }
};
} // namespace

// Every tag in jieba.dict.utf8 must be ≤ 4 characters (fits in PosTag).
TEST_F(PosTagDictTest, AllTagsFitInPosTag) {
    for (const auto &e : entries_) {
        EXPECT_LE(e.tag_str.size(), PosTag::MaxLength) << "Tag '" << e.tag_str << "' for word '" << e.word
                                                       << "' exceeds PosTag::MaxLength (" << PosTag::MaxLength << ")";
    }
}

// Every tag round-trips: string → PosTag → string produces the original.
TEST_F(PosTagDictTest, AllTagsRoundTrip) {
    for (const auto &e : entries_) {
        auto tag = PosTag{e.tag_str};
        auto recovered = tag.to_string();
        EXPECT_EQ(recovered, e.tag_str) << "Round-trip failed for word '" << e.word << "': '" << e.tag_str
                                        << "' → PosTag → '" << recovered << "'";
    }
}

// No two distinct tag strings map to the same PosTag raw value (no collisions).
TEST_F(PosTagDictTest, NoCollisions) {
    auto raw_to_tag = std::unordered_map<uint32_t, std::string>{};
    for (const auto &e : entries_) {
        auto tag = PosTag{e.tag_str};
        auto &&[it, inserted] = raw_to_tag.try_emplace(tag.raw(), e.tag_str);
        if (!inserted) {
            // Same raw value must correspond to the same tag string.
            EXPECT_EQ(it->second, e.tag_str)
                << "Collision: raw=" << tag.raw() << " maps to both '" << it->second << "' and '" << e.tag_str << "'";
        }
    }
}

// All tags are non-empty.
TEST_F(PosTagDictTest, AllTagsNonEmpty) {
    for (const auto &e : entries_) {
        auto tag = PosTag{e.tag_str};
        EXPECT_FALSE(tag.empty()) << "Tag for word '" << e.word << "' produced an empty PosTag";
    }
}

// All tags only contain lowercase ASCII letters (a-z).
TEST_F(PosTagDictTest, AllTagsAreLowercaseAscii) {
    for (const auto &e : entries_) {
        for (auto &&c : e.tag_str) {
            EXPECT_GE(c, 'a') << "Non-lowercase char in tag '" << e.tag_str << "' for word '" << e.word << "'";
            EXPECT_LE(c, 'z') << "Non-lowercase char in tag '" << e.tag_str << "' for word '" << e.word << "'";
        }
    }
}

// Summary: print unique tag count and list.
TEST_F(PosTagDictTest, PrintUniqueTagSummary) {
    std::set<std::string> unique_tags;
    for (const auto &e : entries_) {
        unique_tags.insert(e.tag_str);
    }
    std::cout << "[PosTagDictTest] " << entries_.size() << " entries, " << unique_tags.size() << " unique tags: ";
    for (const auto &t : unique_tags) {
        std::cout << t << " ";
    }
    std::cout << std::endl;

    // Sanity: we expect around 55 unique tags.
    EXPECT_GE(unique_tags.size(), 50u);
    EXPECT_LE(unique_tags.size(), 100u);
}
