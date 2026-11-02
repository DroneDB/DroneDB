/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A remote raster that a tile or a thumbnail reads: its download (the size it may have, the name
// of the copy) and the conversion of the copy, against a server of the test on the loopback
// interface.

#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "loopback_server.h"
#include "mio.h"
#include "raster_cache.h"
#include "raster_helper.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "tilerhelper.h"
#include "userprofile.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    TEST(RemoteInput, theExtensionOfTheCopyIsTheOneOfThePathOfTheUrl)
    {
        EXPECT_EQ(raster::urlFileExtension("https://host/a/ortho.tif"), ".tif");
        EXPECT_EQ(raster::urlFileExtension("https://host/ortho.TIF#part"), ".TIF");
        EXPECT_EQ(raster::urlFileExtension("https://host/ortho.tif?token=a.png&b=c/d.jpg"), ".tif");
        EXPECT_EQ(raster::urlFileExtension("http://host:8080/x/cloud.laz?x#y.z"), ".laz");

        for (const char *none : {"https://host/a/ortho", "https://host/a.d/ortho", "https://host.example.com",
                                 "https://host.example.com/", "https://host:8080", "https://host/.tif",
                                 "https://host/a.t:if", "https://host/a.ti%2Ff", "https://host/a.",
                                 "https://host/a.abcdefghijklmnopq"})
            EXPECT_EQ(raster::urlFileExtension(none), "") << none;
    }

#ifndef _WIN32
    std::string bytesOf(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // A reply whose length the server does not announce: the body ends when the connection does
    std::string unannounced(const std::string &body)
    {
        return "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + body;
    }

    TEST(RemoteInput, aDownloadOfMoreThanTheLimitIsRefusedAndLeavesNothing)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path target = ta.getPath("input.tif");
        const std::string body(4096, 'x');
        testutils::LoopbackServer server({testutils::http(200, "OK", body), unannounced(body),
                                          testutils::http(200, "OK", body)});

        // Announced: refused when the length comes
        EXPECT_THROW(raster::downloadInput(server.url("/a.tif"), target, 1000), NetException);
        EXPECT_FALSE(fs::exists(target));
        // Not announced: refused when the limit is reached
        EXPECT_THROW(raster::downloadInput(server.url("/a.tif"), target, 1000), NetException);
        EXPECT_FALSE(fs::exists(target));
        // Up to the limit
        raster::downloadInput(server.url("/a.tif"), target, body.size());
        EXPECT_EQ(bytesOf(target), body);
    }

    // The real program, counting the COG conversions
    class CountingCogHelper : public raster::IRasterHelper
    {
      public:
        raster::ProbeResult probe(const fs::path &path, const raster::ProbeParams &params) override
        {
            return real_.probe(path, params);
        }
        void thumbnail(const fs::path &path, const fs::path &out, const raster::ThumbnailParams &params) override
        {
            real_.thumbnail(path, out, params);
        }
        void normalize(const fs::path &path, const fs::path &out, const json &params) override
        {
            real_.normalize(path, out, params);
        }
        void geoproject(const fs::path &path, const fs::path &out, const json &params) override
        {
            real_.geoproject(path, out, params);
        }
        void buildCog(const fs::path &path, const fs::path &out, const json &params,
                      const raster::ProgressCallback &progress) override
        {
            cogRequests++;
            real_.buildCog(path, out, params, progress);
        }
        void buildVector(const fs::path &path, const fs::path &out, const json &params,
                         const raster::ProgressCallback &progress) override
        {
            real_.buildVector(path, out, params, progress);
        }

        std::atomic<int> cogRequests{0};

      private:
        raster::GdalUtilHelper real_;
    };

    std::vector<fs::path> filesIn(const fs::path &folder)
    {
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(folder))
            if (entry.is_regular_file())
                files.push_back(entry.path());
        return files;
    }

    // A remote raster whose version the caller does not know is downloaded on every request, and
    // converted once per content: an unchanged file is not converted again, a changed one is
    TEST(RemoteInput, anUnchangedRemoteRasterIsConvertedOnce)
    {
        TestArea ta(TEST_NAME, true);
        const ScopedProcessConfig config(json{{"cacheDir", io::toUtf8(ta.getPath("cache"))}}.dump());
        CountingCogHelper helper;
        const raster::ScopedRasterHelper installed(helper);

        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 64;
        const std::string first = bytesOf(testutils::createGeoRaster(ta.getPath("first.tif"), spec));
        spec.value = [](int band, int x, int y) { return static_cast<double>((x + y * band) % 200); };
        const std::string second = bytesOf(testutils::createGeoRaster(ta.getPath("second.tif"), spec));
        ASSERT_NE(first, second);

        testutils::LoopbackServer server(
            {testutils::http(200, "OK", first), testutils::http(200, "OK", first), testutils::http(200, "OK", second)});
        const fs::path url = io::fromUtf8(server.url("/remote/ortho.tif"));

        const fs::path cog = TilerHelper::toGeoTIFF(url);
        RasterCache::instance().clear();
        EXPECT_EQ(TilerHelper::toGeoTIFF(url), cog);
        EXPECT_EQ(helper.cogRequests, 1);
        EXPECT_EQ(server.requests.size(), 2u) << "downloaded on every request";

        const fs::path changed = TilerHelper::toGeoTIFF(url);
        EXPECT_NE(changed, cog);
        EXPECT_EQ(helper.cogRequests, 2);

        // One copy of the URL, the latest, with the extension of its path
        const auto copies = filesIn(UserProfile::get()->getTilesDir());
        ASSERT_EQ(copies.size(), 1u);
        EXPECT_EQ(copies[0].extension(), ".tif");
        EXPECT_EQ(bytesOf(copies[0]), second);
        RasterCache::instance().clear();
    }
#endif

} // namespace
