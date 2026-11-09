/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// What geoProject writes for the CLI (overwrite as --overwrite gives it, D-028): a missing
// folder named with a trailing separator is made, a file is written, and a projected file that
// exists is refused unless replacing it is asked for. The refusals that need no photo are in
// the unit tests of the CLI (CliOutputSafety).

#include <fstream>
#include <iterator>
#include <string>

#include "exceptions.h"
#include "geoproject.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"

namespace
{
    using namespace ddb;

    std::string readBytes(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    TEST(GeoprojOutput, aFolderNamedWithASeparatorIsMadeAndItsFileIsReplacedOnlyWithOverwrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path photo = ta.downloadTestAsset(testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");
        const fs::path projected = ta.getPath("projected");
        const fs::path written = projected / "DJI_0018.tif";

        // "ddb geoproj projected/ DJI_0018.JPG" with projected/ missing
        geoProject({io::toUtf8(photo)}, io::toUtf8(projected) + "/", "5%", true, nullptr, false);
        ASSERT_TRUE(fs::is_regular_file(written));
        const std::string first = readBytes(written);

        // A second run refuses the file, which stays as it was
        EXPECT_THROW(geoProject({io::toUtf8(photo)}, io::toUtf8(projected) + "/", "5%", true, nullptr, false),
                     OutputExistsException);
        EXPECT_EQ(readBytes(written), first);

        // With overwrite it is written again
        std::ofstream(written, std::ios::binary) << "keep";
        geoProject({io::toUtf8(photo)}, io::toUtf8(projected), "5%", true, nullptr, true);
        EXPECT_NE(readBytes(written), "keep");
    }

    TEST(GeoprojOutput, oneImageToAFileIsWrittenOnceWithoutOverwrite)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path photo = ta.downloadTestAsset(testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");
        const fs::path out = ta.getPath("out.tif");

        geoProject({io::toUtf8(photo)}, io::toUtf8(out), "5%", true, nullptr, false);
        ASSERT_TRUE(fs::is_regular_file(out));
        EXPECT_THROW(geoProject({io::toUtf8(photo)}, io::toUtf8(out), "5%", true, nullptr, false), OutputExistsException);
        EXPECT_NO_THROW(geoProject({io::toUtf8(photo)}, io::toUtf8(out), "5%", true, nullptr, true));

        // Its own image is never the output
        EXPECT_THROW(geoProject({io::toUtf8(photo)}, io::toUtf8(photo), "5%", true, nullptr, true), InvalidArgsException);
    }

} // namespace
