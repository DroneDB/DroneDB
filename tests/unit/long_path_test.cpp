/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A build in a dataset whose root is over 200 characters long, so that the paths of its temporary
// files pass the 260 characters of MAX_PATH. Windows only: on other systems there is no such
// limit. The test process takes long paths because it is declared long path aware
// (cmake/LongPathAware.cmake), and only where Windows allows them, which the test reads from the
// registry (windowsLongPathsEnabled) and skips when it does not: it cannot make the system allow them.

#ifdef _WIN32

#include <algorithm>
#include <filesystem>
#include <string>

#include "ddb.h"
#include "gtest/gtest.h"
#include "utils.h"

namespace {

// A folder of `segments` levels of 40 characters each, relative to the test area
std::filesystem::path deepFolder(int segments) {
    std::filesystem::path folder;
    for (int i = 0; i < segments; i++)
        folder /= std::string(39, 'd') + std::to_string(i);
    return folder;
}

TEST(LongPaths, aPointCloudIsBuiltInADatasetUnderARootOfOverTwoHundredCharacters) {
    if (!windowsLongPathsEnabled())
        GTEST_SKIP() << "Windows has long paths off (LongPathsEnabled is not 1 in "
                        "HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem), so no program can "
                        "open a path of 260 characters or more";

    PointCloudDataset ds("LongPaths.copc", deepFolder(5));
    const std::string root = ds.root();
    ASSERT_GT(root.size(), 200u) << root;

    ASSERT_EQ(DDBBuildEx(root.c_str(), "cloud.xyz", nullptr, true, false, nullptr), DDBERR_NONE)
        << DDBGetLastError();

    // The build wrote its COPC under the root, over the limit
    bool found = false;
    size_t longest = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(ds.index.db->buildDirectory())) {
        longest = std::max(longest, entry.path().native().size());
        found = found || entry.path().extension() == ".laz";
    }
    EXPECT_TRUE(found) << "no COPC under " << ds.index.db->buildDirectory();
    EXPECT_GT(longest, 260u);
}

}  // namespace

#endif  // _WIN32
