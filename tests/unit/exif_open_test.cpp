/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// openExivImage answers the same on every platform and for every path. On Windows a path that is
// not ASCII is read into memory (Exiv2 opens files through the narrow API), and Exiv2's memory
// overload returns nullptr for data of no known image type where its path overload throws: a
// ".jpg" that is not an image then failed "Cannot open" in ddb add on Windows only, while
// everywhere else the indexer catches the Exiv2::Error and indexes the file without EXIF.

#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "exif.h"
#include "mio.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    fs::path writeText(TestArea &area, const std::string &utf8Name)
    {
        const fs::path file = area.getFolder() / io::fromUtf8(utf8Name);
        std::ofstream(file, std::ios::binary) << "not an image";
        return file;
    }

    TEST(ExifOpen, dataOfNoImageTypeIsAnExivErrorWithAnAsciiName)
    {
        TestArea area("ExifOpen.ascii");
        EXPECT_THROW(openExivImage(writeText(area, "plain.jpg")), Exiv2::Error);
    }

    TEST(ExifOpen, dataOfNoImageTypeIsAnExivErrorWithANonAsciiName)
    {
        TestArea area("ExifOpen.nonAscii");
        EXPECT_THROW(openExivImage(writeText(area, "\xC3\xA9t\xC3\xA9.jpg")), Exiv2::Error);
    }
}
