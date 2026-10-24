/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-13: the DSM service in its three parts. The downloader answers from a fake IHttpClient (no
// network), the memory cache and the folder loader read small GeoTIFFs written by the test, and
// DSMService runs over both with its own client and cache folder: no user profile, no OpenTopography.

#include <fstream>
#include <sstream>
#include <string>

#include "constants.h"
#include "dsm_cache.h"
#include "dsm_downloader.h"
#include "dsm_reader.h"
#include "dsmservice.h"
#include "exceptions.h"
#include "fake_http_client.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // The point of every test: inside the raster below, at pixel (5, 57)
    constexpr double Lat = 46.84260708333;
    constexpr double Lon = -91.99455988889;
    constexpr float ExpectedElevation = 5.0f + 57.0f * 1000.0f;

    // A DSM that covers [-92, -91.8] x [46.7, 46.9]; the value of a pixel is x + 1000 * y
    fs::path writeDsm(const fs::path &path, double originX = -92.0, double originY = 46.9)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 200;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = 4326;
        spec.originX = originX;
        spec.originY = originY;
        spec.pixelSize = 0.001;
        spec.value = [](int, int x, int y) { return x + 1000.0 * y; };
        return testutils::createGeoRaster(path, spec);
    }

    std::string slurp(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // The DSM service as the tests reach it: a template whose URL ends with the key, so the
    // fake can route it
    constexpr const char *Template = "https://dsm.test/dem?west={west}&south={south}&east={east}&north={north}&key={key}";
    constexpr const char *Key = "S3cretDsmKey0123";

    void serveDsm(fake::HttpClient &http, const fs::path &dsm)
    {
        http.on("DOWNLOAD", std::string("key=") + Key, fake::reply(200, slurp(dsm)));
    }

    double parameter(const std::string &url, const std::string &name)
    {
        const auto at = url.find(name + "=");
        EXPECT_NE(at, std::string::npos) << url;
        return std::stod(url.substr(at + name.size() + 1));
    }

    // ---------------------------------------------------------------- the downloader

    TEST(Dq13DsmDownloader, WritesTheAnswerToANewFileAndAsksForASquareAroundThePoint)
    {
        TestArea ta(TEST_NAME, true);
        const auto dsm = writeDsm(ta.getPath("served.tif"));
        fake::HttpClient http;
        serveDsm(http, dsm);

        DsmDownload request;
        request.latitude = Lat;
        request.longitude = Lon;
        request.urlTemplate = Template;
        request.apiKey = Key;
        request.folder = ta.getFolder("cache");
        const std::string path = DsmDownloader(http).download(request);

        ASSERT_EQ(http.calls.size(), 1u);
        const std::string url = http.calls[0].request.url;
        EXPECT_LT(parameter(url, "west"), Lon);
        EXPECT_GT(parameter(url, "east"), Lon);
        EXPECT_LT(parameter(url, "south"), Lat);
        EXPECT_GT(parameter(url, "north"), Lat);
        EXPECT_NEAR(parameter(url, "east") - parameter(url, "west"), 0.13, 0.02) << "10 km at 47 degrees";
        EXPECT_EQ(url.find('{'), std::string::npos) << url;

        EXPECT_EQ(fs::path(io::fromUtf8(path)).extension(), ".tif");
        EXPECT_EQ(fs::path(io::fromUtf8(path)).parent_path(), request.folder);
        EXPECT_EQ(slurp(io::fromUtf8(path)), slurp(dsm));
    }

    TEST(Dq13DsmDownloader, WithoutAKeyNothingIsRequested)
    {
        TestArea ta(TEST_NAME, true);
        fake::HttpClient http;

        DsmDownload request;
        request.latitude = Lat;
        request.longitude = Lon;
        request.urlTemplate = Template;
        request.folder = ta.getFolder("cache");
        EXPECT_THROW(DsmDownloader(http).download(request), NetException);
        EXPECT_TRUE(http.calls.empty());
    }

    TEST(Dq13DsmDownloader, AFailedTransferLeavesNoFileAndNeverNamesTheKey)
    {
        TestArea ta(TEST_NAME, true);
        fake::HttpClient http;
        http.on("DOWNLOAD", std::string("key=") + Key, fake::reply(500, "boom"));

        DsmDownload request;
        request.latitude = Lat;
        request.longitude = Lon;
        request.urlTemplate = Template;
        request.apiKey = Key;
        request.folder = ta.getFolder("cache");

        std::string message;
        try
        {
            DsmDownloader(http).download(request);
            FAIL() << "a 500 was taken for a DSM";
        }
        catch (const NetException &e)
        {
            message = e.what();
        }
        EXPECT_EQ(message.find(Key), std::string::npos) << message;
        EXPECT_NE(message.find("dsm.test"), std::string::npos) << "the error still names the service: " << message;
        EXPECT_NE(message.find("500"), std::string::npos) << message;
        EXPECT_TRUE(fs::is_empty(request.folder));
    }

    TEST(Dq13DsmDownloader, ATransportErrorAndAnEmptyAnswerAreNetExceptions)
    {
        TestArea ta(TEST_NAME, true);
        fake::HttpClient http;
        DsmDownload request;
        request.latitude = Lat;
        request.longitude = Lon;
        request.urlTemplate = Template;
        request.apiKey = Key;
        request.folder = ta.getFolder("cache");

        http.on("DOWNLOAD", std::string("key=") + Key, fake::transportFailure());
        EXPECT_THROW(DsmDownloader(http).download(request), NetException);

        http.on("DOWNLOAD", std::string("key=") + Key, fake::reply(200, ""));
        EXPECT_THROW(DsmDownloader(http).download(request), NetException);
        EXPECT_TRUE(fs::is_empty(request.folder));
    }

    // ---------------------------------------------------------------- the caches

    TEST(Dq13DsmCache, AMemoryCacheAnswersFromTheRasterThatContainsThePoint)
    {
        TestArea ta(TEST_NAME, true);
        DsmMemoryCache memory;
        EXPECT_FALSE(memory.lookup(Lat, Lon).has_value());

        EXPECT_TRUE(addRasterToCache(memory, writeDsm(ta.getPath("a.tif")), Lat, Lon));
        EXPECT_TRUE(memory.contains("a.tif"));
        const auto elevation = memory.lookup(Lat, Lon);
        ASSERT_TRUE(elevation.has_value());
        EXPECT_FLOAT_EQ(*elevation, ExpectedElevation);

        // Outside the raster: nothing holds the point
        EXPECT_FALSE(memory.lookup(Lat + 10, Lon).has_value());

        memory.erase("a.tif");
        EXPECT_FALSE(memory.contains("a.tif"));
        EXPECT_FALSE(memory.lookup(Lat, Lon).has_value());
    }

    TEST(Dq13DsmCache, ARasterThatDoesNotContainThePointIsKeptWithoutItsPixels)
    {
        TestArea ta(TEST_NAME, true);
        DsmMemoryCache memory;
        const auto far = writeDsm(ta.getPath("far.tif"), 10.0, 20.0);

        EXPECT_FALSE(addRasterToCache(memory, far, Lat, Lon));
        EXPECT_TRUE(memory.contains("far.tif"));
        EXPECT_FALSE(memory.lookup(Lat, Lon).has_value());
        // Its pixels were not read, so it answers nothing, not even for a point inside it
        EXPECT_FALSE(memory.lookup(19.9, 10.1).has_value());
    }

    TEST(Dq13DsmCache, AFolderIsReadUntilARasterContainsThePointAndBrokenFilesAreDeleted)
    {
        TestArea ta(TEST_NAME, true);
        const auto folder = ta.getFolder("cache");
        writeDsm(folder / "far.tif", 10.0, 20.0);
        writeDsm(folder / "near.tif");
        { std::ofstream(folder / "broken.tif") << "not a raster"; }

        DsmMemoryCache memory;
        // The order of the folder is the file system's: whichever comes first, the point is found
        // and the broken file is deleted when it is reached
        bool found = false;
        for (int attempt = 0; attempt < 3 && !found; attempt++)
            found = loadDsmFolder(folder, memory, Lat, Lon);
        EXPECT_TRUE(found);
        ASSERT_TRUE(memory.lookup(Lat, Lon).has_value());
        EXPECT_FLOAT_EQ(*memory.lookup(Lat, Lon), ExpectedElevation);

        // A second pass over a complete cache has nothing to read
        for (int attempt = 0; attempt < 3; attempt++)
            loadDsmFolder(folder, memory, Lat, Lon);
        EXPECT_FALSE(fs::exists(folder / "broken.tif"));
        EXPECT_TRUE(fs::exists(folder / "near.tif"));
        EXPECT_TRUE(fs::exists(folder / "far.tif"));
    }

    // ---------------------------------------------------------------- the service

    struct ServiceFixture
    {
        TestArea ta;
        fs::path folder;
        fake::HttpClient http;

        explicit ServiceFixture(const std::string &name) : ta(name, true), folder(ta.getFolder("cache")) {}

        DSMService service()
        {
            const auto cache = folder;
            return DSMService(http, [cache] { return cache; });
        }
    };

    TEST(Dq13DsmService, DownloadsOnceThenServesTheElevationFromMemory)
    {
        EnvVarGuard keyGuard(DSM_SERVICE_KEY_ENV, Key);
        ServiceFixture f(TEST_NAME);
        // The default template is OpenTopography's: its URL ends with the key
        f.http.on("DOWNLOAD", std::string("API_Key=") + Key, fake::reply(200, slurp(writeDsm(f.ta.getPath("served.tif")))));
        auto service = f.service();

        EXPECT_FLOAT_EQ(service.getAltitude(Lat, Lon), ExpectedElevation);
        EXPECT_EQ(f.http.calls.size(), 1u);
        EXPECT_FLOAT_EQ(service.getAltitude(Lat, Lon), ExpectedElevation);
        EXPECT_EQ(f.http.calls.size(), 1u) << "the second lookup must not ask again";
    }

    TEST(Dq13DsmService, ANewServiceOverTheSameFolderReadsTheDiskCacheWithoutTheNetwork)
    {
        EnvVarGuard keyGuard(DSM_SERVICE_KEY_ENV, Key);
        ServiceFixture f(TEST_NAME);
        f.http.on("DOWNLOAD", std::string("API_Key=") + Key, fake::reply(200, slurp(writeDsm(f.ta.getPath("served.tif")))));
        {
            auto first = f.service();
            ASSERT_FLOAT_EQ(first.getAltitude(Lat, Lon), ExpectedElevation);
        }
        ASSERT_EQ(f.http.calls.size(), 1u);

        auto second = f.service();
        EXPECT_FLOAT_EQ(second.getAltitude(Lat, Lon), ExpectedElevation);
        EXPECT_EQ(f.http.calls.size(), 1u) << "the folder had the raster";
    }

    TEST(Dq13DsmService, OfflineAndKeylessLookupsFallBackToZeroWithoutThrowing)
    {
        ServiceFixture f(TEST_NAME);
        auto service = f.service();

        {
            EnvVarGuard keyGuard(DSM_SERVICE_KEY_ENV);
            unsetEnv(DSM_SERVICE_KEY_ENV);
            EXPECT_FLOAT_EQ(service.getAltitude(Lat, Lon), 0.0f);
            EXPECT_TRUE(f.http.calls.empty()) << "no key, no request";
        }
        {
            EnvVarGuard keyGuard(DSM_SERVICE_KEY_ENV, Key);
            f.http.on("DOWNLOAD", std::string("API_Key=") + Key, fake::transportFailure());
            EXPECT_FLOAT_EQ(service.getAltitude(Lat, Lon), 0.0f);
            EXPECT_EQ(f.http.calls.size(), 1u);
        }
    }

    TEST(Dq13DsmService, ADownloadOfARasterThatMissesThePointEndsWithZero)
    {
        EnvVarGuard keyGuard(DSM_SERVICE_KEY_ENV, Key);
        ServiceFixture f(TEST_NAME);
        f.http.on("DOWNLOAD", std::string("API_Key=") + Key,
                  fake::reply(200, slurp(writeDsm(f.ta.getPath("served.tif"), 10.0, 20.0))));
        auto service = f.service();
        EXPECT_FLOAT_EQ(service.getAltitude(Lat, Lon), 0.0f);
        EXPECT_EQ(f.http.calls.size(), 1u);
    }
}
