/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef USER_OPEN_CHILD_H
#define USER_OPEN_CHILD_H

#include <functional>

#include "gtest/gtest.h"

// The test binaries are hosts: they never declare themselves the raster helper, so in them
// openUserRaster, openUserVector and identifyUserRaster throw (gdal_open_policy.h) and a host
// code path that parses a file of a user with GDAL fails its test. A test whose subject is the
// code that does open user files (the open policy, the operations of the helper run in the
// process of the test) runs its body in a child process that declares itself the helper first:
// the one place of the test code that calls enableUserFileOpens().
namespace testutils
{
    // Runs `body` in a forked child where enableUserFileOpens() was called, waits for it, and
    // reports in the calling test what the child's test recorded: its failures (with the file
    // and line they were raised at), its skip, an uncaught exception, a crash. The child
    // shares everything the test set up before the call (the cache folder, the installed
    // raster helper, the hooks that make a GDAL write fail), and what it changes in the
    // process stays there.
    //
    // Windows has no fork: the body runs in the process of the test, which then stays a
    // helper for the tests that follow (the ctest runs of that platform use a process per test).
    void runWithUserFileOpens(const std::function<void()> &body);
} // namespace testutils

// TEST whose body runs as runWithUserFileOpens says
#define USER_OPEN_TEST(suite, name)                                                      \
    static void suite##_##name##_UserOpenBody();                                         \
    TEST(suite, name)                                                                    \
    {                                                                                    \
        ::testutils::runWithUserFileOpens(&suite##_##name##_UserOpenBody);               \
    }                                                                                    \
    static void suite##_##name##_UserOpenBody()

// TEST_F whose body runs as runWithUserFileOpens says; SetUp and TearDown run in the parent
#define USER_OPEN_TEST_F(fixture, name)                                                  \
    class fixture##_##name##_UserOpen : public fixture                                   \
    {                                                                                    \
      public:                                                                            \
        void UserOpenBody();                                                             \
    };                                                                                   \
    GTEST_TEST_(fixture, name, fixture##_##name##_UserOpen,                              \
                ::testing::internal::GetTypeId<fixture>())                               \
    {                                                                                    \
        ::testutils::runWithUserFileOpens([this] { UserOpenBody(); });                  \
    }                                                                                    \
    void fixture##_##name##_UserOpen::UserOpenBody()

#endif // USER_OPEN_CHILD_H
