/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "registryutils.h"
#include "constants.h"
#include "exceptions.h"

namespace
{

    using namespace ddb;

    TEST(parseTag, Normal)
    {
        auto s = RegistryUtils::parseTag("test:3000/myorg/myds");
        EXPECT_STREQ(s.registryUrl.c_str(), "https://test:3000");
        EXPECT_STREQ(s.organization.c_str(), "myorg");
        EXPECT_STREQ(s.dataset.c_str(), "myds");

        s = RegistryUtils::parseTag("test/myorg/myds", true);
        EXPECT_STREQ(s.registryUrl.c_str(), "http://test");

        s = RegistryUtils::parseTag("myorg/myds");
        EXPECT_STREQ(s.registryUrl.c_str(), "https://" DEFAULT_REGISTRY);
        EXPECT_STREQ(s.organization.c_str(), "myorg");
        EXPECT_STREQ(s.dataset.c_str(), "myds");

        EXPECT_THROW(
            RegistryUtils::parseTag("myorg"),
            InvalidArgsException);
    }

    TEST(parseTag, ValidNames)
    {
        // Valid: starts with lowercase letter
        auto s = RegistryUtils::parseTag("abc/def");
        EXPECT_STREQ(s.organization.c_str(), "abc");
        EXPECT_STREQ(s.dataset.c_str(), "def");

        // Valid: starts with digit
        s = RegistryUtils::parseTag("0rg/9dataset");
        EXPECT_STREQ(s.organization.c_str(), "0rg");
        EXPECT_STREQ(s.dataset.c_str(), "9dataset");

        // Valid: contains underscores
        s = RegistryUtils::parseTag("my_org/my_dataset");
        EXPECT_STREQ(s.organization.c_str(), "my_org");
        EXPECT_STREQ(s.dataset.c_str(), "my_dataset");

        // Valid: contains dashes
        s = RegistryUtils::parseTag("my-org/my-dataset");
        EXPECT_STREQ(s.organization.c_str(), "my-org");
        EXPECT_STREQ(s.dataset.c_str(), "my-dataset");

        // Valid: mixed valid characters
        s = RegistryUtils::parseTag("org_123-test/ds_456-data");
        EXPECT_STREQ(s.organization.c_str(), "org_123-test");
        EXPECT_STREQ(s.dataset.c_str(), "ds_456-data");

        // Valid: minimum length (2 characters)
        s = RegistryUtils::parseTag("ab/cd");
        EXPECT_STREQ(s.organization.c_str(), "ab");
        EXPECT_STREQ(s.dataset.c_str(), "cd");

        // Valid: maximum length (129 characters)
        std::string maxOrg(129, 'a');
        std::string maxDs(129, 'b');
        s = RegistryUtils::parseTag(maxOrg + "/" + maxDs);
        EXPECT_EQ(s.organization.length(), 129);
        EXPECT_EQ(s.dataset.length(), 129);
    }

    TEST(parseTag, InvalidStartCharacter)
    {
        // Invalid: starts with underscore
        EXPECT_THROW(
            RegistryUtils::parseTag("_org/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/_dataset"),
            InvalidArgsException);

        // Invalid: starts with dash
        EXPECT_THROW(
            RegistryUtils::parseTag("-org/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/-dataset"),
            InvalidArgsException);

        // Invalid: starts with uppercase letter (gets converted to lowercase, but would fail if uppercase was in original)
        // Note: parseTag converts to lowercase, so "Org" becomes "org" which is valid
        // The JavaScript validation would catch this at input time
    }

    TEST(parseTag, InvalidCharacters)
    {
        // Invalid: contains uppercase (will be converted to lowercase by parseTag)
        // But if original validation should reject uppercase, this needs to be tested differently

        // Invalid: contains period
        EXPECT_THROW(
            RegistryUtils::parseTag("org.name/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/data.set"),
            InvalidArgsException);

        // Invalid: contains spaces (will be trimmed, but space in middle is invalid)
        EXPECT_THROW(
            RegistryUtils::parseTag("my org/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/my dataset"),
            InvalidArgsException);

        // Invalid: contains special characters
        EXPECT_THROW(
            RegistryUtils::parseTag("org@name/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/dataset#123"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org$/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/dataset%"),
            InvalidArgsException);
    }

    TEST(parseTag, InvalidLength)
    {
        // Invalid: too short (1 character)
        EXPECT_THROW(
            RegistryUtils::parseTag("a/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/b"),
            InvalidArgsException);

        // Invalid: too long (130 characters)
        std::string tooLongOrg(130, 'a');
        std::string tooLongDs(130, 'b');

        EXPECT_THROW(
            RegistryUtils::parseTag(tooLongOrg + "/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/" + tooLongDs),
            InvalidArgsException);
    }

    TEST(parseTag, EmptyComponents)
    {
        // Invalid: empty organization
        EXPECT_THROW(
            RegistryUtils::parseTag("/dataset"),
            InvalidArgsException);

        // Invalid: empty dataset
        EXPECT_THROW(
            RegistryUtils::parseTag("org/"),
            InvalidArgsException);

        // Invalid: both empty
        EXPECT_THROW(
            RegistryUtils::parseTag("/"),
            InvalidArgsException);
    }

    TEST(parseTag, EdgeCases)
    {
        // Valid: all lowercase letters
        auto s = RegistryUtils::parseTag("abcdefghijklmnopqrstuvwxyz/dataset");
        EXPECT_STREQ(s.organization.c_str(), "abcdefghijklmnopqrstuvwxyz");

        // Valid: all digits
        s = RegistryUtils::parseTag("0123456789/dataset");
        EXPECT_STREQ(s.organization.c_str(), "0123456789");

        // Valid: underscores and dashes after first char
        s = RegistryUtils::parseTag("a______/b------");
        EXPECT_STREQ(s.organization.c_str(), "a______");
        EXPECT_STREQ(s.dataset.c_str(), "b------");

        // Valid: mixed with numbers
        s = RegistryUtils::parseTag("a1b2c3/d4e5f6");
        EXPECT_STREQ(s.organization.c_str(), "a1b2c3");
        EXPECT_STREQ(s.dataset.c_str(), "d4e5f6");
    }

}
