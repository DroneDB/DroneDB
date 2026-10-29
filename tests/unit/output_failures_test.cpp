/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A write that fails is an error. GDAL keeps the pixels of a written raster in its block
// cache and writes them when the dataset is flushed or closed, so a full disk (or a file that
// cannot grow) shows at the close, after every call before it went well. The exporters flushed
// without looking at the result and closed the output by letting it go out of scope: the export
// reported success over a truncated file.
//
// The failure is a real one: the process may not write a file larger than a few KiB (RLIMIT_FSIZE),
// which fails the write(2) that GDAL's close makes (file_size_limit.h). POSIX only; the Windows
// build skips them.

#include <cstdint>
#include <string>
#include <vector>

#include "artefact_cache.h"
#include "exceptions.h"
#include "file_size_limit.h"
#include "gtest/gtest.h"
#include "mask.h"
#include "mio.h"
#include "raster_export.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

#define SKIP_WITHOUT_FILE_SIZE_LIMIT() \
    if (!FileSizeLimit::supported)     \
    GTEST_SKIP() << "RLIMIT_FSIZE is POSIX only"

    // A raster of broadband texture, so that its pixels do not compress to nothing
    fs::path texturedRaster(const fs::path &path, int bands)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 256;
        spec.bands = bands;
        spec.value = [](int band, int x, int y) {
            uint32_t h = static_cast<uint32_t>(y) * 73856093u ^ static_cast<uint32_t>(x + band * 7) * 19349663u;
            h ^= h >> 13;
            h *= 0x85ebca6bu;
            h ^= h >> 16;
            return 20.0 + 200.0 * ((h & 0xFFFFFFu) / static_cast<double>(0x1000000u));
        };
        return testutils::createGeoRaster(path, spec);
    }

    // The raster of the test is read as a user's raster is: from the copy the raster helper writes.
    // The copy is made before the limit, which is meant for the output of the call.
    std::string copiedBeforeTheLimit(const fs::path &raster)
    {
        ddb::raster::artefactFor(raster);
        return io::toUtf8(raster);
    }

    TEST(OutputFailures, ABandExportThatCannotWriteItsFileIsAnError)
    {
        SKIP_WITHOUT_FILE_SIZE_LIMIT();
        TestArea ta(TEST_NAME, true);
        RasterExport request;
        request.input = copiedBeforeTheLimit(texturedRaster(ta.getPath("in.tif"), 3));
        request.output = io::toUtf8(ta.getPath("out.tif"));

        {
            const FileSizeLimit limit(SmallFileLimit);
            EXPECT_THROW(exportRaster(request), AppException);
        }
        EXPECT_FALSE(fs::exists(ta.getPath("out.tif"))) << "a failed export leaves no output";

        // The same export without the limit works: the failure above was the limit's
        EXPECT_NO_THROW(exportRaster(request));
        EXPECT_TRUE(fs::exists(ta.getPath("out.tif")));
    }

    TEST(OutputFailures, AFormulaExportThatCannotWriteItsFileIsAnError)
    {
        SKIP_WITHOUT_FILE_SIZE_LIMIT();
        TestArea ta(TEST_NAME, true);
        RasterExport request;
        request.input = copiedBeforeTheLimit(texturedRaster(ta.getPath("in.tif"), 3));
        request.output = io::toUtf8(ta.getPath("out.tif"));
        request.formula = "VARI";
        request.bandFilter = "RGB";
        request.colormap = "rdylgn";

        {
            const FileSizeLimit limit(SmallFileLimit);
            EXPECT_THROW(exportRaster(request), AppException);
        }
        EXPECT_FALSE(fs::exists(ta.getPath("out.tif"))) << "a failed export leaves no output";

        EXPECT_NO_THROW(exportRaster(request));
        EXPECT_TRUE(fs::exists(ta.getPath("out.tif")));
    }

    TEST(OutputFailures, MaskedBordersThatCannotBeWrittenAreAnError)
    {
        SKIP_WITHOUT_FILE_SIZE_LIMIT();
        TestArea ta(TEST_NAME, true);
        const auto input = copiedBeforeTheLimit(texturedRaster(ta.getPath("in.tif"), 3));
        const auto output = io::toUtf8(ta.getPath("out.tif"));

        {
            const FileSizeLimit limit(SmallFileLimit);
            EXPECT_THROW(maskBorders(input, output), AppException);
        }
        // Without the limit the same call writes the mask
        fs::remove(ta.getPath("out.tif"));
        EXPECT_NO_THROW(maskBorders(input, output));
        EXPECT_TRUE(fs::exists(ta.getPath("out.tif")));
    }
} // namespace
