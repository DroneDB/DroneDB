/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-05: the shared string and math helpers (stringutil.h, mathutil.h).

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "mathutil.h"
#include "mio.h"
#include "randomutil.h"
#include "stringutil.h"

namespace {

using namespace ddb;
using Strings = std::vector<std::string>;

// UTF-8 text: "Café ÀBC €9". Every byte of a multi-byte sequence has the high bit set, so a
// plain char is negative: <cctype> on it is undefined behaviour (MSVC debug builds assert).
const std::string kUtf8 = "Caf\xc3\xa9 \xc3\x80" "BC \xe2\x82\xac" "9";

// ---------------------------------------------------------------- characters

TEST(Dq05Strings, CharHelpersAcceptEveryByte) {
    for (int b = 0; b < 256; b++) {
        const char c = static_cast<char>(b);
        const bool ascii = b < 0x80;
        // A byte of a multi-byte sequence is never a letter, digit or space, and never changes
        if (!ascii) {
            EXPECT_EQ(str::toLowerChar(c), c) << b;
            EXPECT_EQ(str::toUpperChar(c), c) << b;
            EXPECT_FALSE(str::isDigitChar(c)) << b;
            EXPECT_FALSE(str::isAlphaChar(c)) << b;
            EXPECT_FALSE(str::isAlnumChar(c)) << b;
            EXPECT_FALSE(str::isUpperChar(c)) << b;
            EXPECT_FALSE(str::isSpaceChar(c)) << b;
        }
    }
    EXPECT_EQ(str::toLowerChar('Q'), 'q');
    EXPECT_EQ(str::toUpperChar('q'), 'Q');
    EXPECT_TRUE(str::isDigitChar('7'));
    EXPECT_TRUE(str::isAlphaChar('x'));
    EXPECT_TRUE(str::isAlnumChar('x') && str::isAlnumChar('0'));
    EXPECT_FALSE(str::isAlnumChar('_'));
    EXPECT_TRUE(str::isUpperChar('A'));
    for (char c : {' ', '\t', '\n', '\v', '\f', '\r'})
        EXPECT_TRUE(str::isSpaceChar(c));
}

// ---------------------------------------------------------------- case

TEST(Dq05Strings, CaseConversionLeavesNonAsciiBytesAlone) {
    std::string lower = kUtf8;
    str::toLower(lower);
    EXPECT_EQ(lower, "caf\xc3\xa9 \xc3\x80" "bc \xe2\x82\xac" "9");
    std::string upper = kUtf8;
    str::toUpper(upper);
    EXPECT_EQ(upper, "CAF\xc3\xa9 \xc3\x80" "BC \xe2\x82\xac" "9");
    EXPECT_EQ(str::toLowerCopy(kUtf8), lower);
    EXPECT_EQ(str::toUpperCopy(kUtf8), upper);
    EXPECT_EQ(str::toLowerCopy(""), "");
}

TEST(Dq05Strings, IsLowerCase) {
    EXPECT_TRUE(str::isLowerCase(""));
    EXPECT_TRUE(str::isLowerCase("abc-123_\xc3\x80"));  // "À" is not an ASCII letter
    EXPECT_FALSE(str::isLowerCase("abC"));
}

TEST(Dq05Strings, IequalsAndAffixesIgnoreAsciiCase) {
    EXPECT_TRUE(str::iequals("", ""));
    EXPECT_TRUE(str::iequals("PnG", "png"));
    EXPECT_FALSE(str::iequals("png", "pn"));
    EXPECT_FALSE(str::iequals("png", "pnh"));
    EXPECT_FALSE(str::iequals("\xc3\x80", "\xc3\xa0"));  // not folded: that is utf8.h

    EXPECT_TRUE(str::endsWithCI("x.COPC.LAZ", ".copc.laz"));
    EXPECT_TRUE(str::endsWithCI(".copc.laz", ".copc.laz"));
    EXPECT_TRUE(str::endsWithCI("anything", ""));
    EXPECT_FALSE(str::endsWithCI("c.laz", ".copc.laz"));  // shorter than the suffix
    EXPECT_FALSE(str::endsWithCI("x.copc.las", ".copc.laz"));

    EXPECT_TRUE(str::startsWithCI("FILE:///x", "file:"));
    EXPECT_FALSE(str::startsWithCI("fil", "file:"));
    EXPECT_FALSE(str::startsWithCI("http://", "file:"));
}

// ---------------------------------------------------------------- trim

TEST(Dq05Strings, TrimRemovesAsciiWhitespaceOnly) {
    EXPECT_EQ(str::trimCopy("  a b \t\r\n"), "a b");
    EXPECT_EQ(str::trimCopy("\v\fa\f\v"), "a");
    EXPECT_EQ(str::trimCopy(""), "");
    EXPECT_EQ(str::trimCopy(" \t\r\n"), "");

    // NBSP (C2 A0) bytes are not ASCII spaces
    const std::string nbsp = "\xc2\xa0 x \xc2\xa0";
    EXPECT_EQ(str::trimCopy(nbsp), nbsp);
    std::string padded = nbsp;
    str::trim(padded);
    EXPECT_EQ(padded, nbsp);

    std::string s = "  hi  ";
    str::trim(s);
    EXPECT_EQ(s, "hi");
}

TEST(Dq05Strings, TrimViewWithCustomCharacters) {
    EXPECT_EQ(str::trimView("xx-a-xx", "x"), "-a-");
    EXPECT_EQ(str::trimView("\"quoted\"", "\""), "quoted");
    EXPECT_EQ(str::trimView("   ", " "), "");
    // The view points into the argument
    const std::string text = "  ab ";
    EXPECT_EQ(str::trimView(text).data(), text.data() + 2);
}

// ---------------------------------------------------------------- split / join / replace

TEST(Dq05Strings, SplitKeepsEmptyPieces) {
    EXPECT_EQ(str::split("a,b,c", ","), (Strings{"a", "b", "c"}));
    EXPECT_EQ(str::split("a,,b", ","), (Strings{"a", "", "b"}));
    EXPECT_EQ(str::split(",a,", ","), (Strings{"", "a", ""}));
    EXPECT_EQ(str::split("", ","), (Strings{""}));
    EXPECT_EQ(str::split("abc", ","), (Strings{"abc"}));
    EXPECT_EQ(str::split("a::b::c", "::"), (Strings{"a", "b", "c"}));
    EXPECT_EQ(str::split("a:::b", "::"), (Strings{"a", ":b"}));  // left to right, no overlap
    EXPECT_EQ(str::split("a;b", ';'), (Strings{"a", "b"}));
}

TEST(Dq05Strings, SplitWithEmptyDelimiterReturnsTheWholeText) {
    EXPECT_EQ(str::split("a,b", ""), (Strings{"a,b"}));
    EXPECT_EQ(str::split("", ""), (Strings{""}));
}

TEST(Dq05Strings, SplitWhitespaceSkipsRuns) {
    EXPECT_EQ(str::splitWhitespace("  a \t b\r\n c  "), (Strings{"a", "b", "c"}));
    EXPECT_EQ(str::splitWhitespace(""), Strings{});
    EXPECT_EQ(str::splitWhitespace(" \t\n"), Strings{});
    EXPECT_EQ(str::splitWhitespace("one"), (Strings{"one"}));
    EXPECT_EQ(str::splitWhitespace("a\xc2\xa0" "b c"), (Strings{"a\xc2\xa0" "b", "c"}));
}

// join() removed the separator after the last item with pop_back(), also when there was no
// item: pop_back() on an empty string is undefined behaviour (a segfault on libstdc++).
// Registry::downloadFiles joins its list unchecked.
TEST(Dq05Strings, JoinOfNoItemsIsEmpty) {
    EXPECT_EQ(str::join({}), "");
    EXPECT_EQ(str::join({"a"}), "a");
    EXPECT_EQ(str::join({""}), "");
    EXPECT_EQ(str::join({"a", "b", ""}, ";"), "a;b;");
    EXPECT_EQ(str::join({"a", "b"}), "a,b");
    EXPECT_EQ(str::join({"a", "b"}, ", "), "a, b");
}

TEST(Dq05Strings, ReplaceAll) {
    std::string s = "a/b%c*d";
    str::replaceAll(s, "/", "//");
    str::replaceAll(s, "%", "/%");
    str::replaceAll(s, "*", "%");
    EXPECT_EQ(s, "a//b/%c%d");

    std::string x = "xx";
    str::replaceAll(x, "x", "yx");  // the replacement is not scanned again
    EXPECT_EQ(x, "yxyx");

    std::string unchanged = "abc";
    str::replaceAll(unchanged, "", "z");
    EXPECT_EQ(unchanged, "abc");
    str::replaceAll(unchanged, "q", "z");
    EXPECT_EQ(unchanged, "abc");

    std::string shrink = "aaaa";
    str::replaceAll(shrink, "aa", "");
    EXPECT_EQ(shrink, "");
}

// ---------------------------------------------------------------- the extension rule

TEST(Dq05Strings, LowerExtensionIsTheOneExtensionRule) {
    EXPECT_EQ(io::lowerExtension("a/B.TIF"), "tif");
    EXPECT_EQ(io::lowerExtension("x.copc.laz"), "laz");
    EXPECT_EQ(io::lowerExtension("noext"), "");
    EXPECT_EQ(io::lowerExtension(".hidden"), "");
    EXPECT_EQ(io::lowerExtension("dir.d/file"), "");
    EXPECT_EQ(io::lowerExtension(io::fromUtf8("caf\xc3\xa9.JPG")), "jpg");

    EXPECT_TRUE(io::Path("a/B.TIF").checkExtension({"png", "TIF"}));
    EXPECT_FALSE(io::Path("a/B.TIF").checkExtension({"png"}));
    EXPECT_FALSE(io::Path("noext").checkExtension({"png", ""}));
}

// ---------------------------------------------------------------- random names

// The alphabet of the old generateRandomString skipped 'V' ("...STUWXYZ")
TEST(Dq05Strings, RandomStringUsesLettersAndDigitsOnly) {
    EXPECT_EQ(randomString(0), "");
    EXPECT_EQ(randomString(16).size(), 16u);

    std::set<char> seen;
    for (int i = 0; i < 400; i++)
        for (char c : randomString(64)) {
            EXPECT_TRUE(str::isAlnumChar(c)) << static_cast<int>(c);
            seen.insert(c);
        }
    EXPECT_EQ(seen.size(), 62u);  // 25600 draws: every letter and digit shows up
    EXPECT_TRUE(seen.count('V'));
}

// ---------------------------------------------------------------- math

TEST(Dq05Math, PiAndAngleConversions) {
    static_assert(kPi == 3.14159265358979323846);
    EXPECT_EQ(deg2rad(180.0), kPi);
    EXPECT_EQ(rad2deg(kPi), 180.0);
    EXPECT_DOUBLE_EQ(rad2deg(deg2rad(33.3)), 33.3);
    EXPECT_EQ(deg2rad(0.0), 0.0);
}

TEST(Dq05Math, SameFloat) {
    EXPECT_TRUE(sameFloat(1.0f, 1.0f));
    EXPECT_TRUE(sameFloat(1.0f, 1.0f + 5e-7f));
    EXPECT_FALSE(sameFloat(1.0f, 1.001f));
    EXPECT_FALSE(sameFloat(-9999.0f, -9998.0f));
    EXPECT_FALSE(sameFloat(std::nanf(""), std::nanf("")));
}

TEST(Dq05Math, IsPowerOfTwo) {
    EXPECT_TRUE(isPowerOfTwo(1));
    EXPECT_TRUE(isPowerOfTwo(256u));
    EXPECT_FALSE(isPowerOfTwo(0));
    EXPECT_FALSE(isPowerOfTwo(-4));
    EXPECT_FALSE(isPowerOfTwo(6));
}

}  // namespace
