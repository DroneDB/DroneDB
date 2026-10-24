/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <fstream>
#include "mzip.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{

    using namespace ddb;

    TEST(zip, extractAllRestoresTheArchivedTree)
    {
        TestArea ta(TEST_NAME);

        fs::path dir = ta.getFolder("zipTest");
        {
            std::ofstream output((dir / "a.txt").string());
        }
        fs::create_directories(dir / "subdir");
        {
            std::ofstream output((dir / "subdir" / "b.txt").string());
        }

        std::string zipFile = (ta.getFolder(".") / "archive.zip").string();
        fs::remove(zipFile);
        zipFolder(dir.string(), zipFile);

        fs::path outdir = ta.getFolder("zipOutput");
        ddb::zip::extractAll(zipFile, outdir.string());

        EXPECT_TRUE(fs::is_regular_file(outdir / "a.txt"));
        EXPECT_TRUE(fs::is_directory(outdir / "subdir"));
        EXPECT_TRUE(fs::is_regular_file(outdir / "subdir" / "b.txt"));
    }

} // namespace
