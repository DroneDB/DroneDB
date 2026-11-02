/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A file that is not an image has no metadata, whatever its name and whichever platform: the
// reader answers "no metadata" for an ASCII and for a non-ASCII name alike, and the indexer
// then indexes the file without EXIF. (On Windows a non-ASCII path is read into memory by the
// metadata helper, and Exiv2's memory overload returns nullptr for data of no known image type
// where its path overload throws: a ".jpg" that is not an image once failed "Cannot open" in
// ddb add on Windows only.)

#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "exif.h"
#include "exif_metadata.h"
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

    void expectNoMetadata(const fs::path &file)
    {
        const auto result = exif::metadataReader().read(file);
        EXPECT_FALSE(result.metadata.has_value());
        EXPECT_FALSE(result.message.empty());
        EXPECT_EQ(readExifParser(file), nullptr);
    }

    TEST(ExifOpen, dataOfNoImageTypeHasNoMetadataWithAnAsciiName)
    {
        TestArea area("ExifOpen.ascii");
        expectNoMetadata(writeText(area, "plain.jpg"));
    }

    TEST(ExifOpen, dataOfNoImageTypeHasNoMetadataWithANonAsciiName)
    {
        TestArea area("ExifOpen.nonAscii");
        expectNoMetadata(writeText(area, "\xC3\xA9t\xC3\xA9.jpg"));
    }
}
