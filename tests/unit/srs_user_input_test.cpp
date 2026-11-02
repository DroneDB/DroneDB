/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A coordinate system a user writes (the CRS of a drawing, the SRS of a region request, a .prj
// next to a drawing) is read from its text alone: GDAL would also read a file the text names or
// fetch a URL, from the host process.

#include <string>
#include <vector>

#include "cad.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "loopback_server.h"
#include "mio.h"
#include "raster_region.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // Whether every way a user's text reaches GDAL refuses `input`
    void expectRefusedEverywhere(const std::string &input)
    {
        SCOPED_TRACE(input);
        EXPECT_EQ(srsFromUserInput(input.c_str()), nullptr);
        OGRSpatialReference srs;
        EXPECT_NE(setSrsFromUserInput(srs, input.c_str()), OGRERR_NONE);
        EXPECT_THROW(requireSrsUserInput(input, "SRS"), InvalidArgsException);
        EXPECT_THROW(cad::prjContent(input), InvalidArgsException);

        char *out = nullptr;
        EXPECT_NE(DDBMakeCadPrj(input.c_str(), &out), DDBERR_NONE);
        DDBFree(out);

        const double bbox[4] = {0, 0, 1, 1};
        uint8_t *bytes = nullptr;
        int size = 0;
        EXPECT_THROW(renderRasterRegion(testutils::regionRequest("missing.tif", bbox, input, 8, 8, "png"), {}, &bytes,
                                        &size),
                     InvalidArgsException);
        RegionOptions options;
        options.outputCrs = input;
        EXPECT_THROW(
            renderRasterRegion(testutils::regionRequest("missing.tif", bbox, "EPSG:4326", 8, 8, "png"), options, &bytes,
                               &size),
            InvalidArgsException);
    }

    TEST(SrsUserInput, aCoordinateSystemIsReadFromItsText)
    {
        EXPECT_NE(srsFromUserInput("EPSG:32632"), nullptr);
        EXPECT_NE(srsFromUserInput("+proj=longlat +datum=WGS84 +no_defs"), nullptr);
        EXPECT_NE(srsFromUserInput("http://www.opengis.net/def/crs/EPSG/0/4326"), nullptr) << "an OGC name, not a fetch";
        EXPECT_NO_THROW(requireSrsUserInput("EPSG:4326", "SRS"));
        const std::string wkt = cad::prjContent("EPSG:32632");
        EXPECT_NE(srsFromUserInput(wkt.c_str()), nullptr);
        EXPECT_NE(cad::prjContent(wkt), "");
    }

    TEST(SrsUserInput, aFileThatTheTextNamesIsNotRead)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path prj = writeFile(ta.getPath("crs.prj"), cad::prjContent("EPSG:32632"));
        expectRefusedEverywhere(io::toUtf8(prj));
    }

#ifndef _WIN32
    TEST(SrsUserInput, aUrlThatTheTextNamesIsNotFetched)
    {
        const std::string wkt = cad::prjContent("EPSG:32632");
        testutils::LoopbackServer server({testutils::http(200, "OK", wkt)});

        expectRefusedEverywhere(server.url("/crs.prj"));
        expectRefusedEverywhere("/vsicurl/" + server.url("/crs.prj"));
        EXPECT_TRUE(server.requests.empty()) << server.requests.size() << " requests reached the server";
    }
#endif

} // namespace
