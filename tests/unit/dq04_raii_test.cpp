/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-04: the RAII resource layer (gdal_raii.h, io::ScopedTempDir) and the leaks it closes.

#include <string>

#include "align.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // validateAlignRaster opened both rasters, then returned without closing the reference
    // when the source could not be opened.
    TEST(Dq04Raii, Target_ValidateAlignClosesTheReferenceWhenTheSourceCannotBeOpened)
    {
        TestArea ta(TEST_NAME, true);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        spec.bands = 1;
        const fs::path reference = testutils::createGeoRaster(ta.getPath("reference.tif"), spec);

        const int before = openDatasetCount();
        const auto result =
            validateAlignRaster(ta.getPath("missing.tif").string(), reference.string());
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(openDatasetCount(), before);
    }
}
