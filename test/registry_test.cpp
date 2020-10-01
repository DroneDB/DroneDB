/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "registryutils.h"
#include "constants.h"
#include "exceptions.h"

namespace{

using namespace ddb;

TEST(parseTag, Normal) {
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
        InvalidArgsException
    );
}

}
