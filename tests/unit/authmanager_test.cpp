/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// ~/.ddb/auth.json holds the registry passwords (base64-encoded): it must be readable by its
// owner only, and written so that it is never half there.

#include <fstream>
#include <string>

#include "authmanager.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    size_t filesIn(const fs::path &folder)
    {
        size_t n = 0;
        for (const auto &e : fs::directory_iterator(folder))
        {
            (void)e;
            n++;
        }
        return n;
    }

#ifndef _WIN32
    fs::perms modeOf(const fs::path &p) { return fs::status(p).permissions() & fs::perms::mask; }
    constexpr fs::perms OwnerOnly = fs::perms::owner_read | fs::perms::owner_write;
#endif

    TEST(AuthManager, savedCredentialsRoundTrip)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("auth.json");
        {
            AuthManager am(file);
            am.saveCredentials("https://example.test", AuthCredentials("user", "pass-word"));
        }
        AuthManager reopened(file);
        const AuthCredentials ac = reopened.loadCredentials("https://example.test");
        EXPECT_EQ(ac.username, "user");
        EXPECT_EQ(ac.password, "pass-word");
        // The temporary sibling of the atomic write is gone
        EXPECT_EQ(filesIn(file.parent_path()), 1u);
    }

    // The password is whatever follows the first colon
    TEST(AuthManager, passwordWithColonsRoundTrips)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("auth.json");
        for (const std::string password : {"pass:word", "a:b:c", ":leading", "trailing:", "::"})
        {
            SCOPED_TRACE(password);
            {
                AuthManager am(file);
                am.saveCredentials("https://example.test", AuthCredentials("user", password));
            }
            const AuthCredentials ac = AuthManager(file).loadCredentials("https://example.test");
            EXPECT_EQ(ac.username, "user");
            EXPECT_EQ(ac.password, password);
        }
    }

#ifndef _WIN32
    TEST(AuthManager, newFileIsOwnerOnly)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("auth.json");
        AuthManager am(file);
        am.saveCredentials("https://example.test", AuthCredentials("user", "pass"));
        EXPECT_EQ(modeOf(file), OwnerOnly);
    }

    TEST(AuthManager, wideFileIsTightenedOnTheNextSave)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("auth.json");
        {
            std::ofstream out(file);
            out << R"({"auths":{}})";
        }
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                  fs::perms::others_read,
                        fs::perm_options::replace);
        ASSERT_EQ(modeOf(file), fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                    fs::perms::others_read);

        AuthManager am(file);
        am.saveCredentials("https://example.test", AuthCredentials("user", "pass"));
        EXPECT_EQ(modeOf(file), OwnerOnly);
        EXPECT_EQ(am.loadCredentials("https://example.test").username, "user");

        // Deleting rewrites the file too
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::others_read,
                        fs::perm_options::replace);
        EXPECT_TRUE(am.deleteCredentials("https://example.test"));
        EXPECT_EQ(modeOf(file), OwnerOnly);
    }
#endif

    // A failed write must say so (it used to be silent: the credentials were lost) and leave no
    // temporary file behind
    TEST(AuthManager, failedSaveThrows)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path file = ta.getPath("missing-folder") / "auth.json";
        AuthManager am(file); // a profile folder that cannot be written is no error until a save
        EXPECT_THROW(am.saveCredentials("https://example.test", AuthCredentials("user", "pass")), FSException);
        EXPECT_FALSE(fs::exists(file));
    }
} // namespace
