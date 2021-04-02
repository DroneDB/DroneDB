/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <syncmanager.h>

#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;

TEST(syncManager, happyPath) {
    TestArea ta(TEST_NAME);

    SyncManager manager(ta.getFolder().string());

    const auto t = time(nullptr);

    manager.setLastSync(t, "testhub.dronedb.app");

    const auto newt = manager.getLastSync("testhub.dronedb.app");

    EXPECT_EQ(t, newt);

}
}  // namespace
