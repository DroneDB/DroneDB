/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A file a user supplied is opened with the GDAL drivers its extension allows. A VRT, which
// lists other files and URLs to read, must not be reachable through a .tif or .geojson name,
// and a VRT that is opened is checked first. The files below are the ones of an attack: a VRT
// named .tif that reads a raster outside the dataset, one that requests a URL, an OGR VRT named
// .geojson, a VRT with a Python pixel function, a VRT that climbs out of its folder.

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "cpl_http.h"
#include "entry.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "gdal_rat.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"
#include "user_open_child.h"
#include "vector.h"

namespace
{
    using namespace ddb;

    void write(const fs::path &path, const std::string &text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    fs::path writeGeoTiff(const fs::path &path)
    {
        fs::create_directories(path.parent_path());
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = 32615;
        spec.originX = 576648.71;
        spec.originY = 5188209.785;
        spec.pixelSize = 0.05;
        spec.value = [](int, int x, int y) { return 100.0 + x + y; };
        return testutils::createGeoRaster(path, spec);
    }

    // A raster in another GDAL format, with the data of writeGeoTiff
    fs::path writeAs(const char *driver, const fs::path &path)
    {
        const fs::path source = writeGeoTiff(path.string() + ".source.tif");
        const auto src = adoptDataset(GDALOpen(io::toUtf8(source).c_str(), GA_ReadOnly));
        EXPECT_NE(src, nullptr);
        auto out = adoptDataset(GDALCreateCopy(GDALGetDriverByName(driver), io::toUtf8(path).c_str(),
                                               datasetHandle(src), FALSE, nullptr, nullptr, nullptr));
        EXPECT_NE(out, nullptr) << driver;
        out.reset();
        fs::remove(source);
        return path;
    }

    // The WKT of the sources below: the georeferencing is not what the tests are about
    const char *Srs = "EPSG:32615";

    // A VRT with one band that reads `source`
    std::string vrtOf(const std::string &source, const char *relativeToVrt = "1")
    {
        return std::string("<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\">\n  <SRS>") + Srs +
               "</SRS>\n  <GeoTransform>576648.71, 0.05, 0, 5188209.785, 0, -0.05</GeoTransform>\n"
               "  <VRTRasterBand dataType=\"Float32\" band=\"1\">\n    <NoDataValue>-9999</NoDataValue>\n"
               "    <ComplexSource>\n      <SourceFilename relativeToVRT=\"" +
               relativeToVrt + "\">" + source +
               "</SourceFilename>\n      <SourceBand>1</SourceBand>\n"
               "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"32\" ySize=\"32\" />\n"
               "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"32\" ySize=\"32\" />\n"
               "    </ComplexSource>\n  </VRTRasterBand>\n</VRTDataset>\n";
    }

    // An OGR VRT: the layer "stolen" is the first layer of the data source at `source`
    std::string ogrVrtOf(const std::string &source)
    {
        return "<OGRVRTDataSource><OGRVRTLayer name=\"stolen\"><SrcDataSource>" + source +
               "</SrcDataSource><SrcLayer>secret</SrcLayer></OGRVRTLayer></OGRVRTDataSource>";
    }

    // A folder with a dataset inside and a secret raster and a secret GeoJSON next to it
    struct Scene
    {
        TestArea area;
        fs::path dataset;
        fs::path outsideTif;
        fs::path outsideGeoJson;

        explicit Scene(const std::string &name)
            : area(name, true), dataset(area.getFolder("dataset")), outsideTif(writeGeoTiff(area.getFolder("outside") / "secret.tif"))
        {
            outsideGeoJson = area.getFolder("outside") / "secret.geojson";
            write(outsideGeoJson,
                  R"({"type":"FeatureCollection","name":"secret","features":[{"type":"Feature","properties":{"secret":"outside-data"},"geometry":{"type":"Point","coordinates":[10.0,45.0]}}]})");
        }
    };

    EntryType typeOf(const fs::path &file, const fs::path &root, Entry *out = nullptr)
    {
        Entry e;
        parseEntry(file, root, e, false);
        if (out != nullptr)
            *out = e;
        return e.type;
    }

    // Records the URLs GDAL requests on this thread while it lives; each request fails, so
    // nothing leaves the host
    class HttpRecorder
    {
      public:
        HttpRecorder()
        {
            CPLHTTPPushFetchCallback(&HttpRecorder::fetch, this);
        }
        ~HttpRecorder()
        {
            CPLHTTPPopFetchCallback();
        }
        HttpRecorder(const HttpRecorder &) = delete;
        HttpRecorder &operator=(const HttpRecorder &) = delete;

        std::vector<std::string> urls;

      private:
        static CPLHTTPResult *fetch(const char *url, CSLConstList, GDALProgressFunc, void *,
                                    CPLHTTPFetchWriteFunc, void *, void *self)
        {
            static_cast<HttpRecorder *>(self)->urls.emplace_back(url);
            auto *result = static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));
            result->nStatus = 1;
            result->pszErrBuf = CPLStrdup("recorded, not sent");
            return result;
        }
    };

    std::string joined(const std::vector<std::string> &urls)
    {
        std::string all;
        for (const auto &u : urls)
            all += u + " ";
        return all;
    }

    bool hasDriver(const char *name)
    {
        return GDALGetDriverByName(name) != nullptr;
    }

    // ---- The files of the attack -------------------------------------------------------------

    USER_OPEN_TEST(gdalOpenPolicy, vrtNamedTifWithAnAbsoluteSourceIsNotARaster)
    {
        Scene s(TEST_NAME);
        const fs::path poc = s.dataset / "disguised.tif";
        write(poc, vrtOf(io::toUtf8(s.outsideTif), "0"));

        EXPECT_EQ(openUserRaster(io::toUtf8(poc)), nullptr);
        EXPECT_NE(typeOf(poc, s.dataset), EntryType::GeoRaster);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtNamedTifWithAnUrlSourceIsNotOpenedNorRequested)
    {
        Scene s(TEST_NAME);
        const fs::path poc = s.dataset / "disguised-url.tif";
        write(poc, vrtOf("/vsicurl/http://127.0.0.1:1/dsm.tif", "0"));

        EXPECT_EQ(openUserRaster(io::toUtf8(poc)), nullptr);
        EXPECT_NE(typeOf(poc, s.dataset), EntryType::GeoRaster);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtNamedJp2AndJ2kAreNotRasters)
    {
        Scene s(TEST_NAME);
        for (const char *name : {"disguised.jp2", "disguised.j2k", "DISGUISED.TIFF"})
        {
            const fs::path poc = s.dataset / name;
            write(poc, vrtOf(io::toUtf8(s.outsideTif), "0"));
            EXPECT_EQ(openUserRaster(io::toUtf8(poc)), nullptr) << name;
            EXPECT_NE(typeOf(poc, s.dataset), EntryType::GeoRaster) << name;
        }
    }

    USER_OPEN_TEST(gdalOpenPolicy, ogrVrtNamedGeoJsonIsNotOpenedNorBuilt)
    {
        Scene s(TEST_NAME);
        const fs::path poc = s.dataset / "disguised.geojson";
        write(poc, ogrVrtOf(io::toUtf8(s.outsideGeoJson)));

        EXPECT_EQ(openUserVector(io::toUtf8(poc)), nullptr);

        Entry e;
        typeOf(poc, s.dataset, &e);
        EXPECT_FALSE(e.properties.contains("vector")) << e.properties.dump();
        EXPECT_EQ(e.polygon_geom.size(), 0);

        const fs::path out = s.area.getFolder("build");
        EXPECT_THROW(buildVector(io::toUtf8(poc), io::toUtf8(out)), AppException);
        EXPECT_FALSE(fs::exists(out / "vec" / "source.gpkg"));
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtWithATraversalIsNotOpened)
    {
        Scene s(TEST_NAME);
        const fs::path poc = s.dataset / "traversal.vrt";
        write(poc, vrtOf("../outside/secret.tif"));

        EXPECT_EQ(openUserRaster(io::toUtf8(poc)), nullptr);
        const auto verdict = gdalopen::validateVrt(poc, s.dataset);
        EXPECT_FALSE(verdict.ok);
        EXPECT_NE(verdict.reason.find("outside"), std::string::npos) << verdict.reason;
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtWithAPythonPixelFunctionIsNotOpenedAndPythonIsOff)
    {
        Scene s(TEST_NAME);
        const fs::path marker = s.area.getFolder("sec") / "PWNED";
        writeGeoTiff(s.dataset / "dsm.tif");
        const fs::path poc = s.dataset / "python-pixel-function.vrt";
        write(poc, std::string("<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\">\n  <SRS>") + Srs +
                       "</SRS>\n  <GeoTransform>576648.71, 0.05, 0, 5188209.785, 0, -0.05</GeoTransform>\n"
                       "  <VRTRasterBand dataType=\"Float32\" band=\"1\" subClass=\"VRTDerivedRasterBand\">\n"
                       "    <PixelFunctionType>pwn</PixelFunctionType>\n"
                       "    <PixelFunctionLanguage>Python</PixelFunctionLanguage>\n"
                       "    <PixelFunctionCode><![CDATA[\nimport numpy\ndef pwn(in_ar, out_ar, *args, **kwargs):\n"
                       "    open(\"" +
                       io::toGenericUtf8(marker) +
                       "\", \"w\").write(\"x\")\n    out_ar[:] = in_ar[0]\n]]></PixelFunctionCode>\n"
                       "    <SimpleSource><SourceFilename relativeToVRT=\"1\">dsm.tif</SourceFilename>"
                       "<SourceBand>1</SourceBand></SimpleSource>\n  </VRTRasterBand>\n</VRTDataset>\n");

        EXPECT_EQ(openUserRaster(io::toUtf8(poc)), nullptr);
        EXPECT_FALSE(gdalopen::validateVrt(poc, s.dataset).ok);
        EXPECT_FALSE(fs::exists(marker));

        // The host environment cannot turn it on
        EXPECT_STREQ(CPLGetConfigOption("GDAL_VRT_ENABLE_PYTHON", ""), "NO");
    }

    USER_OPEN_TEST(gdalOpenPolicy, otherRasterFormatsNamedTifAndJp2AreNotRasters)
    {
        Scene s(TEST_NAME);

        // A GeoPackage raster, the format of a database
        const fs::path gpkg = writeAs("GPKG", s.dataset / "raster.gpkg");
        const fs::path asTif = s.dataset / "fake_gpkg.tif";
        const fs::path asJp2 = s.dataset / "fake_gpkg.jp2";
        fs::copy_file(gpkg, asTif);
        fs::copy_file(gpkg, asJp2);

        for (const auto &f : {asTif, asJp2})
        {
            EXPECT_EQ(openUserRaster(io::toUtf8(f)), nullptr) << f;
            EXPECT_NE(typeOf(f, s.dataset), EntryType::GeoRaster) << f;
        }

        if (hasDriver("netCDF"))
        {
            const fs::path nc = writeAs("netCDF", s.dataset / "raster.nc");
            const fs::path ncAsTif = s.dataset / "fake_nc.tif";
            fs::copy_file(nc, ncAsTif);
            EXPECT_EQ(openUserRaster(io::toUtf8(ncAsTif)), nullptr);
            EXPECT_NE(typeOf(ncAsTif, s.dataset), EntryType::GeoRaster);
        }
    }

    // ---- What keeps opening ------------------------------------------------------------------

    USER_OPEN_TEST(gdalOpenPolicy, rastersKeepOpeningAndKeepTheirType)
    {
        Scene s(TEST_NAME);
        const fs::path tif = writeGeoTiff(s.dataset / "dsm.tif");
        EXPECT_NE(openUserRaster(io::toUtf8(tif)), nullptr);
        EXPECT_EQ(typeOf(tif, s.dataset), EntryType::GeoRaster);

        // The extension is read in any case
        const fs::path upper = s.dataset / "DSM2.TIF";
        fs::copy_file(tif, upper);
        EXPECT_NE(openUserRaster(io::toUtf8(upper)), nullptr);
        EXPECT_EQ(typeOf(upper, s.dataset), EntryType::GeoRaster);

        const fs::path tiff = s.dataset / "dsm.tiff";
        fs::copy_file(tif, tiff);
        EXPECT_NE(openUserRaster(io::toUtf8(tiff)), nullptr);

        // A TIFF without a projection stays an image
        const fs::path plain = s.dataset / "plain.tif";
        {
            const auto created = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(plain).c_str(), 8, 8,
                                                         1, GDT_Byte, nullptr));
            ASSERT_NE(created, nullptr);
        }
        EXPECT_NE(openUserRaster(io::toUtf8(plain)), nullptr);
        EXPECT_EQ(typeOf(plain, s.dataset), EntryType::Image);

        const fs::path jpg = writeAs("JPEG", s.dataset / "photo.jpg");
        EXPECT_NE(openUserRaster(io::toUtf8(jpg)), nullptr);
        EXPECT_EQ(typeOf(jpg, s.dataset), EntryType::Image);

        const fs::path png = writeAs("PNG", s.dataset / "picture.PNG");
        EXPECT_NE(openUserRaster(io::toUtf8(png)), nullptr);
        EXPECT_EQ(typeOf(png, s.dataset), EntryType::Image);

        if (hasDriver("JP2OpenJPEG"))
        {
            testutils::GeoRasterSpec spec;
            spec.width = spec.height = 32;
            const fs::path jp2 = testutils::createGeoJp2(s.dataset / "ortho.jp2", spec);
            EXPECT_NE(openUserRaster(io::toUtf8(jp2)), nullptr);
            EXPECT_EQ(typeOf(jp2, s.dataset), EntryType::GeoRaster);
        }
    }

    USER_OPEN_TEST(gdalOpenPolicy, vectorsKeepOpeningAndKeepTheirType)
    {
        Scene s(TEST_NAME);

        const fs::path geojson = s.dataset / "points.geojson";
        fs::copy_file(s.outsideGeoJson, geojson);
        EXPECT_NE(openUserVector(io::toUtf8(geojson)), nullptr);
        Entry e;
        EXPECT_EQ(typeOf(geojson, s.dataset, &e), EntryType::Vector);
        EXPECT_EQ(e.properties["vector"]["driver"], "GeoJSON");

        // A GeoPackage vector, a shapefile, a GPX
        const fs::path gpkg = s.dataset / "points.gpkg";
        const fs::path shp = s.dataset / "points.shp";
        const auto source = openUserVector(io::toUtf8(geojson));
        ASSERT_NE(source, nullptr);
        for (const auto &[driver, path] : {std::pair<const char *, fs::path>{"GPKG", gpkg}, {"ESRI Shapefile", shp}})
        {
            const auto copy = adoptDataset(GDALCreateCopy(GDALGetDriverByName(driver), io::toUtf8(path).c_str(),
                                                          datasetHandle(source), FALSE, nullptr, nullptr, nullptr));
            ASSERT_NE(copy, nullptr) << driver;
        }
        EXPECT_NE(openUserVector(io::toUtf8(gpkg)), nullptr);
        EXPECT_EQ(typeOf(gpkg, s.dataset), EntryType::Vector);
        EXPECT_NE(openUserVector(io::toUtf8(shp)), nullptr);
        EXPECT_EQ(typeOf(shp, s.dataset), EntryType::Vector);
    }

    USER_OPEN_TEST(gdalOpenPolicy, anExtensionThatIsNotReadIsNotOpened)
    {
        Scene s(TEST_NAME);
        const fs::path asc = s.dataset / "grid.asc";
        write(asc, "ncols 1\nnrows 1\nxllcorner 0\nyllcorner 0\ncellsize 1\n1\n");
        EXPECT_EQ(openUserRaster(io::toUtf8(asc)), nullptr);

        const fs::path noExtension = writeGeoTiff(s.dataset / "dsm.tif");
        fs::copy_file(noExtension, s.dataset / "noextension");
        EXPECT_EQ(openUserRaster(io::toUtf8(s.dataset / "noextension")), nullptr);

        // A raster is not a vector and a vector is not a raster
        EXPECT_EQ(openUserVector(io::toUtf8(noExtension)), nullptr);
        EXPECT_EQ(openUserRaster(io::toUtf8(s.outsideGeoJson)), nullptr);
    }

    TEST(gdalOpenPolicy, driversAreOnlyThosePresent)
    {
        for (const auto &name : gdalopen::rasterDrivers("a.tif"))
            EXPECT_NE(GDALGetDriverByName(name.c_str()), nullptr) << name;
        EXPECT_FALSE(gdalopen::rasterDrivers("a.TIF").empty());
        EXPECT_TRUE(gdalopen::rasterDrivers("a.nc").empty());
        for (const auto &name : gdalopen::vectorDrivers("a.geojson"))
            EXPECT_NE(name, "OGR_VRT");
        EXPECT_TRUE(gdalopen::vectorDrivers("a.vrt").empty());
    }

    // ---- The VRTs that are opened ------------------------------------------------------------

    USER_OPEN_TEST(gdalOpenPolicy, vrtOverFilesInItsFolderOpens)
    {
        Scene s(TEST_NAME);
        writeGeoTiff(s.dataset / "tiles" / "a.tif");
        const fs::path vrt = s.dataset / "mosaic.vrt";
        write(vrt, vrtOf("tiles/a.tif"));

        const auto verdict = gdalopen::validateVrt(vrt, s.dataset);
        EXPECT_TRUE(verdict.ok) << verdict.reason;
        const auto dataset = openUserRaster(io::toUtf8(vrt));
        ASSERT_NE(dataset, nullptr);
        EXPECT_EQ(dataset->GetRasterXSize(), 32);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtSourcesAreChecked)
    {
        Scene s(TEST_NAME);
        writeGeoTiff(s.dataset / "a.tif");
        const fs::path vrt = s.dataset / "x.vrt";

        struct Case
        {
            const char *what;
            std::string xml;
        };
        const std::vector<Case> refused = {
            {"absolute", vrtOf(io::toUtf8(s.outsideTif), "1")},
            {"not relative to the VRT", vrtOf("a.tif", "0")},
            {"no relativeToVRT", [] {
                 std::string v = vrtOf("a.tif");
                 v.replace(v.find(" relativeToVRT=\"1\""), 18, "");
                 return v;
             }()},
            {"virtual file system", vrtOf("/vsizip//data/a.zip/a.tif")},
            {"URL", vrtOf("https://example.com/a.tif")},
            {"climbs out of the folder", vrtOf("../outside/secret.tif")},
            {"climbs out through a subfolder", vrtOf("sub/../../outside/secret.tif")},
            {"a raw binary file", vrtOf("a.bin")},
            {"no source", vrtOf("")},
        };
        for (const auto &c : refused)
        {
            write(vrt, c.xml);
            const auto verdict = gdalopen::validateVrt(vrt, s.dataset);
            EXPECT_FALSE(verdict.ok) << c.what;
            EXPECT_FALSE(verdict.reason.empty()) << c.what;
            EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr) << c.what;
        }

        // Not a VRT at all, and an OGR VRT, which lists vector files
        write(vrt, "not xml");
        EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
        write(vrt, ogrVrtOf("a.geojson"));
        EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtSourceThatIsAVrtNamedTifIsRefused)
    {
        // A check of names alone accepts outer.vrt -> inner.tif, a VRT that reads the outside
        Scene s(TEST_NAME);
        write(s.dataset / "inner.tif", vrtOf(io::toUtf8(s.outsideTif), "0"));
        const fs::path outer = s.dataset / "outer.vrt";
        write(outer, vrtOf("inner.tif"));

        const auto verdict = gdalopen::validateVrt(outer, s.dataset);
        EXPECT_FALSE(verdict.ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(outer)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtSourceThatIsAVrtIsCheckedInTurn)
    {
        Scene s(TEST_NAME);
        writeGeoTiff(s.dataset / "a.tif");
        write(s.dataset / "inner.vrt", vrtOf("../outside/secret.tif"));
        const fs::path outer = s.dataset / "outer.vrt";
        write(outer, vrtOf("inner.vrt"));
        EXPECT_FALSE(gdalopen::validateVrt(outer, s.dataset).ok);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtNestingStopsAtTwoLevels)
    {
        Scene s(TEST_NAME);
        writeGeoTiff(s.dataset / "a.tif");
        write(s.dataset / "l3.vrt", vrtOf("a.tif"));
        write(s.dataset / "l2.vrt", vrtOf("l3.vrt"));
        write(s.dataset / "l1.vrt", vrtOf("l2.vrt"));
        write(s.dataset / "l0.vrt", vrtOf("l1.vrt"));

        // l0 -> l1 -> l2 -> l3 -> a.tif: a VRT may reach two levels of VRTs below it
        EXPECT_TRUE(gdalopen::validateVrt(s.dataset / "l2.vrt", s.dataset).ok);
        EXPECT_TRUE(gdalopen::validateVrt(s.dataset / "l1.vrt", s.dataset).ok);
        const auto verdict = gdalopen::validateVrt(s.dataset / "l0.vrt", s.dataset);
        EXPECT_FALSE(verdict.ok);
        EXPECT_NE(verdict.reason.find("nested"), std::string::npos) << verdict.reason;
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtSymlinkOutOfTheFolderIsRefused)
    {
        Scene s(TEST_NAME);
        std::error_code ec;
        fs::create_symlink(s.outsideTif, s.dataset / "link.tif", ec);
        if (ec)
            GTEST_SKIP() << "No symbolic links here: " << ec.message();

        const fs::path vrt = s.dataset / "x.vrt";
        write(vrt, vrtOf("link.tif"));
        const auto verdict = gdalopen::validateVrt(vrt, s.dataset);
        EXPECT_FALSE(verdict.ok);
        EXPECT_NE(verdict.reason.find("outside"), std::string::npos) << verdict.reason;
        EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr);
    }

    // ---- The files GDAL opens next to a raster -----------------------------------------------

    // GDAL opens the external overviews (<name>.ovr), the external mask (<name>.msk) and an
    // ERDAS .aux of a raster with all its drivers, whatever the drivers the raster was opened
    // with: a VRT under such a name next to a real GeoTIFF reads what it lists.

    // A raster outside the dataset whose pixels are all `Secret`
    constexpr double Secret = 4242.0;

    fs::path writeSecret(const fs::path &path)
    {
        fs::create_directories(path.parent_path());
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = 32615;
        spec.originX = 576648.71;
        spec.originY = 5188209.785;
        spec.pixelSize = 0.05;
        spec.value = [](int, int, int) { return Secret; };
        return testutils::createGeoRaster(path, spec);
    }

    // Whether any overview or the mask of band 1 holds the secret: what GDAL would serve from
    // a sidecar that reads the outside raster
    bool servesTheSecret(GDALDataset &dataset)
    {
        auto *band = dataset.GetRasterBand(1);
        std::vector<GDALRasterBand *> bands;
        for (int i = 0; i < band->GetOverviewCount(); i++)
            bands.push_back(band->GetOverview(i));
        if (band->GetMaskFlags() == GMF_PER_DATASET)
            bands.push_back(band->GetMaskBand());
        for (auto *b : bands)
        {
            double value = 0;
            if (b != nullptr && b->RasterIO(GF_Read, 0, 0, 1, 1, &value, 1, 1, GDT_Float64, 0, 0) == CE_None &&
                value == Secret)
                return true;
        }
        return false;
    }

    // The sidecar names GDAL looks for, upper case too where the file system tells them apart
    std::vector<std::string> sidecarSuffixes()
    {
        std::vector<std::string> suffixes = {".ovr", ".msk"};
#ifndef _WIN32
        suffixes.insert(suffixes.end(), {".OVR", ".MSK"});
#endif
        return suffixes;
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtNamedAsTheOverviewsOrMaskOfARasterIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");
        const fs::path jpg = writeAs("JPEG", s.dataset / "b.jpg");

        for (const fs::path &raster : {tif, jpg})
        {
            for (const auto &suffix : sidecarSuffixes())
            {
                const std::string sidecar = io::toUtf8(raster) + suffix;
                SCOPED_TRACE(sidecar);
                write(io::fromUtf8(sidecar), vrtOf(io::toUtf8(secret), "0"));

                EXPECT_FALSE(gdalopen::validateSidecars(io::toUtf8(raster)).ok);
                const auto dataset = openUserRaster(io::toUtf8(raster));
                EXPECT_EQ(dataset, nullptr);
                if (dataset)
                {
                    EXPECT_FALSE(servesTheSecret(*dataset));
                }

                // Neither is a thumbnail made of it
                const fs::path thumb = s.area.getPath("thumb.webp");
                fs::remove(thumb);
                EXPECT_ANY_THROW(generateThumb(raster, 64, thumb, true));
                EXPECT_FALSE(fs::exists(thumb));

                fs::remove(io::fromUtf8(sidecar));
            }
        }
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtNamedAsTheOverviewsOfASidecarIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");
        // Real overviews, whose own overviews are the VRT
        writeGeoTiff(s.dataset / "a.tif.ovr");
        write(s.dataset / "a.tif.ovr.ovr", vrtOf(io::toUtf8(secret), "0"));

        EXPECT_FALSE(gdalopen::validateSidecars(io::toUtf8(tif)).ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtBehindAnErdasAuxHeaderIsRefused)
    {
        if (!hasDriver("HFA"))
            GTEST_SKIP() << "No HFA driver";
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");

        // GDAL opens an .aux that starts with the HFA tag with all its drivers, and the VRT
        // driver comes before HFA
        for (const char *name : {"a.aux", "a.tif.aux"})
        {
            SCOPED_TRACE(name);
            write(s.dataset / name, "EHFA_HEADER_TAG " + vrtOf(io::toUtf8(secret), "0"));
            EXPECT_FALSE(gdalopen::validateSidecars(io::toUtf8(tif)).ok);
            EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
            fs::remove(s.dataset / name);
        }

        // An .aux GDAL does not open is not looked at
        write(s.dataset / "a.aux", "notes of another program");
        EXPECT_NE(openUserRaster(io::toUtf8(tif)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtSourceWithAnUnsafeSidecarIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        writeGeoTiff(s.dataset / "a.tif");
        write(s.dataset / "a.tif.ovr", vrtOf(io::toUtf8(secret), "0"));
        const fs::path vrt = s.dataset / "mosaic.vrt";
        write(vrt, vrtOf("a.tif"));

        EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, sidecarThatIsASymbolicLinkIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");
        std::error_code ec;
        fs::create_symlink(writeSecret(s.area.getFolder("outside") / "secret-constant.tif"), s.dataset / "a.tif.ovr",
                           ec);
        if (ec)
            GTEST_SKIP() << "No symbolic links here: " << ec.message();

        EXPECT_FALSE(gdalopen::validateSidecars(io::toUtf8(tif)).ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, geoTiffOverviewsAndMaskNextToARasterKeepWorking)
    {
        Scene s(TEST_NAME);
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");
        {
            // Read-only, GDAL writes the overviews and the mask next to the file, as GeoTIFFs
            auto dataset = adoptDataset(GDALOpen(io::toUtf8(tif).c_str(), GA_ReadOnly));
            ASSERT_NE(dataset, nullptr);
            const int levels[] = {2, 4};
            ASSERT_EQ(dataset->BuildOverviews("AVERAGE", 2, levels, 0, nullptr, nullptr, nullptr, nullptr),
                      CE_None);
            ASSERT_EQ(dataset->CreateMaskBand(GMF_PER_DATASET), CE_None);
        }
        ASSERT_TRUE(fs::exists(s.dataset / "a.tif.ovr"));
        ASSERT_TRUE(fs::exists(s.dataset / "a.tif.msk"));

        const auto verdict = gdalopen::validateSidecars(io::toUtf8(tif));
        EXPECT_TRUE(verdict.ok) << verdict.reason;
        const auto dataset = openUserRaster(io::toUtf8(tif));
        ASSERT_NE(dataset, nullptr);
        EXPECT_EQ(dataset->GetRasterBand(1)->GetOverviewCount(), 2);
        EXPECT_EQ(dataset->GetRasterBand(1)->GetMaskFlags(), GMF_PER_DATASET);
        EXPECT_FALSE(servesTheSecret(*dataset));
    }

    // GDAL resolves only the path after a driver prefix (GTIFF_DIR:1:<path>) against the folder
    // of the VRT, so the prefix must not pass for a folder a ".." climbs out of
    USER_OPEN_TEST(gdalOpenPolicy, vrtSourcesWithADriverPrefixOrInlineAreRefused)
    {
        Scene s(TEST_NAME);
        writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        writeGeoTiff(s.dataset / "a.tif");
        const fs::path vrt = s.dataset / "x.vrt";

        const std::vector<std::string> refused = {
            "GTIFF_DIR:1:../outside/secret-constant.tif",
            "GTIFF_DIR:1:a.tif",
            "GTIFF_RAW:../outside/secret-constant.tif",
            "NITF_IM:a/b/c/d:../../../../../outside/secret-constant.tif",
            "PDF:1:../outside/secret-constant.tif",
            "vrt://a.tif",
            "&lt;VRTDataset rasterXSize=\"32\" rasterYSize=\"32\"&gt;&lt;VRTRasterBand dataType=\"Float32\" "
            "band=\"1\"&gt;&lt;SimpleSource&gt;&lt;SourceFilename relativeToVRT=\"1\"&gt;../outside/"
            "secret-constant.tif&lt;/SourceFilename&gt;&lt;/SimpleSource&gt;&lt;/VRTRasterBand&gt;"
            "&lt;/VRTDataset&gt;",
        };
        for (const auto &source : refused)
        {
            SCOPED_TRACE(source);
            write(vrt, vrtOf(source));
            const auto verdict = gdalopen::validateVrt(vrt, s.dataset);
            EXPECT_FALSE(verdict.ok);
            const auto dataset = openUserRaster(io::toUtf8(vrt));
            EXPECT_EQ(dataset, nullptr);
            if (dataset)
            {
                double value = 0;
                if (dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &value, 1, 1, GDT_Float64, 0, 0) ==
                    CE_None)
                {
                    EXPECT_NE(value, Secret);
                }
            }
        }
    }

#ifndef _WIN32
    // The attack on a POSIX file system: a folder named like the prefix, with a decoy raster
    // inside, makes the source look like a file of the dataset, while GDAL reads
    // <folder of the VRT>/../outside/secret-constant.tif
    USER_OPEN_TEST(gdalOpenPolicy, vrtSourceWithADriverPrefixDoesNotClimbOutBehindADecoyFolder)
    {
        Scene s(TEST_NAME);
        writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        writeGeoTiff(s.dataset / "GTIFF_DIR:1:.." / "outside" / "secret-constant.tif");
        const fs::path vrt = s.dataset / "x.vrt";
        write(vrt, vrtOf("GTIFF_DIR:1:../outside/secret-constant.tif"));

        EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
        const auto dataset = openUserRaster(io::toUtf8(vrt));
        EXPECT_EQ(dataset, nullptr);
        if (dataset)
        {
            double value = 0;
            ASSERT_EQ(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &value, 1, 1, GDT_Float64, 0, 0),
                      CE_None);
            EXPECT_NE(value, Secret);
        }
    }
#endif

    // The GML driver requests the schemas a GML names while it opens it: the schemas its .xsd
    // includes (always, whatever the configuration) and, for a WFS response, the
    // DescribeFeatureType URL of its schemaLocation. A GML of a user names any URL, so opening
    // one must request none.
    USER_OPEN_TEST(gdalOpenPolicy, gmlRequestsNoUrlWhileItOpens)
    {
        if (!hasDriver("GML"))
            GTEST_SKIP() << "No GML driver";
        Scene s(TEST_NAME);
        const std::string feature = "  <gml:featureMember><ns:t><ns:name>a</ns:name><ns:geom><gml:Point "
                                    "srsName=\"EPSG:4326\"><gml:coordinates>10,45</gml:coordinates></gml:Point>"
                                    "</ns:geom></ns:t></gml:featureMember>\n";

        // An .xsd next to the GML that includes a remote schema
        const fs::path withXsd = s.dataset / "included.gml";
        write(withXsd, "<?xml version=\"1.0\"?>\n<ns:FeatureCollection xmlns:ns=\"http://example.com/ns\" "
                       "xmlns:gml=\"http://www.opengis.net/gml\">\n" + feature + "</ns:FeatureCollection>\n");
        write(s.dataset / "included.xsd",
              "<?xml version=\"1.0\"?>\n<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
              "targetNamespace=\"http://example.com/ns\">\n"
              "  <xs:include schemaLocation=\"http://127.0.0.1:9/included-schema.xsd\"/>\n</xs:schema>\n");

        // A WFS response whose schemaLocation is a DescribeFeatureType request
        const fs::path wfs = s.dataset / "wfs.gml";
        write(wfs, "<?xml version=\"1.0\"?>\n<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
                   "xmlns:ns=\"http://example.com/ns\" xmlns:gml=\"http://www.opengis.net/gml\" "
                   "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                   "xsi:schemaLocation=\"http://example.com/ns "
                   "http://127.0.0.1:9/wfs?SERVICE=WFS&amp;REQUEST=DescribeFeatureType&amp;TYPENAME=ns:t\">\n" +
                       feature + "</wfs:FeatureCollection>\n");

        HttpRecorder recorder;
        EXPECT_TRUE(openUserVector(io::toUtf8(withXsd)));
        EXPECT_TRUE(openUserVector(io::toUtf8(wfs)));
        EXPECT_TRUE(recorder.urls.empty()) << joined(recorder.urls);

        // The host environment cannot turn the download of the WFS schema back on
        EXPECT_STREQ(CPLGetConfigOption("GML_DOWNLOAD_SCHEMA", ""), "NO");
    }

    // ---- The datasets the metadata of a raster names -----------------------------------------

    // GDAL opens the dataset the OVERVIEW_FILE item of the OVERVIEWS domain names as the
    // overviews of a raster that has none next to it, with all its drivers, any path or URL,
    // the first time the overviews are asked for: long after the raster opened with the
    // drivers of its extension. A TIFF keeps any metadata domain in its GDAL_METADATA tag and
    // a JPEG 2000 in its GDAL metadata box.

    // Makes the metadata of the GeoTIFF at path name `file` in `item` of `domain`
    fs::path nameInMetadata(const fs::path &path, const char *domain, const char *item, const std::string &file)
    {
        auto dataset = adoptDataset(GDALOpen(io::toUtf8(path).c_str(), GA_Update));
        EXPECT_NE(dataset, nullptr);
        if (dataset)
        {
            EXPECT_EQ(dataset->SetMetadataItem(item, file.c_str(), domain), CE_None);
        }
        return path;
    }

    fs::path writeGeoTiffNaming(const fs::path &path, const char *domain, const char *item, const std::string &file)
    {
        return nameInMetadata(writeGeoTiff(path), domain, item, file);
    }

    // The raster at path opened with `driver` alone, as the drivers of its extension open it
    GDALDatasetUniquePtr openWithDriver(const fs::path &path, const char *driver)
    {
        const char *const only[] = {driver, nullptr};
        return adoptDataset(GDALOpenEx(io::toUtf8(path).c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, only, nullptr,
                                       nullptr));
    }

    std::string itemOf(GDALDataset &dataset, const char *domain, const char *item)
    {
        const char *value = dataset.GetMetadataItem(item, domain);
        return value == nullptr ? std::string() : value;
    }

    void expectNoThumbnail(const fs::path &raster, TestArea &area)
    {
        const fs::path thumb = area.getPath("thumb.webp");
        fs::remove(thumb);
        EXPECT_ANY_THROW(generateThumb(raster, 64, thumb, true));
        EXPECT_FALSE(fs::exists(thumb));
    }

    USER_OPEN_TEST(gdalOpenPolicy, overviewFileInTheMetadataOfARasterIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");

        // An absolute path, and a path relative to the folder of the raster. A raster each: GDAL
        // deletes the overviews with the raster, so writing over the first would delete the
        // outside one.
        const std::pair<const char *, std::string> cases[] = {
            {"a.tif", io::toUtf8(secret)}, {"b.tif", ":::BASE:::../outside/secret-constant.tif"}};
        for (const auto &[name, target] : cases)
        {
            SCOPED_TRACE(target);
            const fs::path tif = writeGeoTiffNaming(s.dataset / name, "OVERVIEWS", "OVERVIEW_FILE", target);
            {
                // What GDAL makes of it: the raster opened as a TIFF alone serves the outside one
                const auto plain = openWithDriver(tif, "GTiff");
                ASSERT_NE(plain, nullptr);
                ASSERT_EQ(itemOf(*plain, "OVERVIEWS", "OVERVIEW_FILE"), target);
                ASSERT_TRUE(servesTheSecret(*plain));
            }

            EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
            expectNoThumbnail(tif, s.area);
            EXPECT_NE(typeOf(tif, s.dataset), EntryType::GeoRaster);
        }
    }

    USER_OPEN_TEST(gdalOpenPolicy, overviewFileInTheMetadataOfASidecarIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");

        for (const char *suffix : {".ovr", ".msk"})
        {
            SCOPED_TRACE(suffix);
            const fs::path sidecar = writeGeoTiffNaming(io::fromUtf8(io::toUtf8(tif) + suffix), "OVERVIEWS",
                                                        "OVERVIEW_FILE", io::toUtf8(secret));
            if (std::string_view(suffix) == ".ovr")
            {
                // The overviews of the overviews are the outside raster
                const auto plain = openWithDriver(tif, "GTiff");
                ASSERT_NE(plain, nullptr);
                ASSERT_TRUE(servesTheSecret(*plain));
            }

            EXPECT_FALSE(gdalopen::validateSidecars(io::toUtf8(tif)).ok);
            EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
            fs::remove(sidecar);
        }
        EXPECT_NE(openUserRaster(io::toUtf8(tif)), nullptr);
    }

#ifndef _WIN32
    // A port on the loopback that accepts connections and never answers: whether a connection
    // is waiting tells whether a request was made
    class SilentListener
    {
      public:
        SilentListener() : fd_(socket(AF_INET, SOCK_STREAM, 0))
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t length = sizeof(address);
            if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr *>(&address), length) != 0 || listen(fd_, 4) != 0 ||
                getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
                throw std::runtime_error("cannot listen on the loopback");
            port_ = ntohs(address.sin_port);
        }
        ~SilentListener()
        {
            if (fd_ >= 0)
                close(fd_);
        }
        SilentListener(const SilentListener &) = delete;
        SilentListener &operator=(const SilentListener &) = delete;

        int port() const
        {
            return port_;
        }
        bool connected() const
        {
            pollfd waiting{fd_, POLLIN, 0};
            return poll(&waiting, 1, 0) > 0;
        }
        // The next connection, waited for up to `milliseconds`; -1 when none came
        int acceptWithin(int milliseconds) const
        {
            pollfd waiting{fd_, POLLIN, 0};
            return poll(&waiting, 1, milliseconds) > 0 ? accept(fd_, nullptr, nullptr) : -1;
        }

      private:
        int fd_;
        int port_ = 0;
    };

    // A port on the loopback that answers every HTTP request with 404 Not Found at once and
    // keeps its request line ("HEAD /a.tif HTTP/1.1")
    class NotFoundServer
    {
      public:
        NotFoundServer() : thread_([this] { serve(); }) {}
        ~NotFoundServer()
        {
            stop_ = true;
            thread_.join();
        }
        NotFoundServer(const NotFoundServer &) = delete;
        NotFoundServer &operator=(const NotFoundServer &) = delete;

        int port() const
        {
            return listener_.port();
        }
        std::vector<std::string> requests() const
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            return requests_;
        }

      private:
        void serve()
        {
            while (!stop_)
            {
                const int client = listener_.acceptWithin(50);
                if (client < 0)
                    continue;
                std::string request;
                char buffer[4096];
                ssize_t received = 0;
                while (request.find("\r\n\r\n") == std::string::npos &&
                       (received = recv(client, buffer, sizeof(buffer), 0)) > 0)
                    request.append(buffer, static_cast<size_t>(received));
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request.substr(0, request.find("\r\n")));
                }
                const std::string_view notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                                  "Connection: close\r\n\r\n";
                CPL_IGNORE_RET_VAL(send(client, notFound.data(), notFound.size(), 0));
                close(client);
            }
        }

        SilentListener listener_;
        mutable std::mutex mutex_;
        std::vector<std::string> requests_;
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };
#endif

    USER_OPEN_TEST(gdalOpenPolicy, overviewFileThatIsAnUrlIsRefusedWithoutARequest)
    {
        Scene s(TEST_NAME);
#ifndef _WIN32
        SilentListener listener;
        const std::string url = "/vsicurl/http://127.0.0.1:" + std::to_string(listener.port()) + "/overviews.tif";
#else
        const std::string url = "/vsicurl/http://127.0.0.1:9/overviews.tif";
#endif
        // A request that is made gives up soon instead of waiting for the silent port
        const ScopedThreadConfigOption timeout("GDAL_HTTP_TIMEOUT", "2");
        const ScopedThreadConfigOption noRetry("GDAL_HTTP_MAX_RETRY", "0");
        const fs::path tif = writeGeoTiffNaming(s.dataset / "a.tif", "OVERVIEWS", "OVERVIEW_FILE", url);

        const auto dataset = openUserRaster(io::toUtf8(tif));
        EXPECT_EQ(dataset, nullptr);
        if (dataset)
            dataset->GetRasterBand(1)->GetOverviewCount();
        expectNoThumbnail(tif, s.area);
#ifndef _WIN32
        EXPECT_FALSE(listener.connected());
#endif
    }

    USER_OPEN_TEST(gdalOpenPolicy, overviewFileInTheMetadataOfAJpeg2000IsRefused)
    {
        if (!hasDriver("JP2OpenJPEG"))
            GTEST_SKIP() << "No JP2OpenJPEG driver";
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        // Bytes, which the JPEG 2000 driver writes
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        spec.bands = 1;
        const fs::path source = nameInMetadata(testutils::createGeoRaster(s.area.getFolder("work") / "source.tif", spec),
                                               "OVERVIEWS", "OVERVIEW_FILE", io::toUtf8(secret));
        const fs::path jp2 = s.dataset / "b.jp2";
        {
            const auto src = openWithDriver(source, "GTiff");
            ASSERT_NE(src, nullptr);
            // A JPEG 2000 keeps the metadata in its GDAL metadata box
            const char *const options[] = {"WRITE_METADATA=YES", "REVERSIBLE=YES", "QUALITY=100", nullptr};
            const auto out = adoptDataset(GDALCreateCopy(GDALGetDriverByName("JP2OpenJPEG"), io::toUtf8(jp2).c_str(),
                                                         datasetHandle(src), FALSE, const_cast<char **>(options),
                                                         nullptr, nullptr));
            ASSERT_NE(out, nullptr);
        }
        {
            const auto plain = openWithDriver(jp2, "JP2OpenJPEG");
            ASSERT_NE(plain, nullptr);
            ASSERT_EQ(itemOf(*plain, "OVERVIEWS", "OVERVIEW_FILE"), io::toUtf8(secret));
            ASSERT_TRUE(servesTheSecret(*plain));
        }

        EXPECT_EQ(openUserRaster(io::toUtf8(jp2)), nullptr);
        expectNoThumbnail(jp2, s.area);
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtSourceWhoseMetadataNamesAnOverviewFileIsRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        writeGeoTiffNaming(s.dataset / "a.tif", "OVERVIEWS", "OVERVIEW_FILE", io::toUtf8(secret));
        const fs::path vrt = s.dataset / "mosaic.vrt";
        write(vrt, vrtOf("a.tif"));

        EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr);

        // A VRT source that is a VRT naming one in its own metadata, opened once it is checked
        const fs::path inner = s.dataset / "inner.vrt";
        std::string innerXml = vrtOf("b.tif");
        innerXml.insert(innerXml.find("  <SRS>"), "  <Metadata domain=\"OVERVIEWS\"><MDI key=\"OVERVIEW_FILE\">" +
                                                      io::toUtf8(secret) + "</MDI></Metadata>\n");
        writeGeoTiff(s.dataset / "b.tif");
        write(inner, innerXml);
        write(vrt, vrtOf("inner.vrt"));
        EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, geolocationArraysInTheMetadataOfARasterAreRefused)
    {
        Scene s(TEST_NAME);
        const fs::path secret = writeSecret(s.area.getFolder("outside") / "secret-constant.tif");

        // A warp of a raster without a geotransform opens them with all the drivers of GDAL
        for (const char *item : {"X_DATASET", "Y_DATASET"})
        {
            SCOPED_TRACE(item);
            const fs::path tif =
                writeGeoTiffNaming(s.dataset / (std::string(item) + ".tif"), "GEOLOCATION", item, io::toUtf8(secret));
            {
                const auto plain = openWithDriver(tif, "GTiff");
                ASSERT_NE(plain, nullptr);
                ASSERT_EQ(itemOf(*plain, "GEOLOCATION", item), io::toUtf8(secret));
            }
            EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
        }
    }

    // A GeoTIFF band reads its attribute table from <name>.vat.dbf, opened with all the vector
    // drivers of GDAL: an OGR VRT under that name reads the layer it lists
    USER_OPEN_TEST(gdalOpenPolicy, attributeTableThatIsNotADbaseTableIsRefused)
    {
        if (!hasDriver("OGR_VRT"))
            GTEST_SKIP() << "No OGR_VRT driver";
        Scene s(TEST_NAME);
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");
        write(s.dataset / "a.tif.vat.dbf", ogrVrtOf(io::toUtf8(s.outsideGeoJson)));
        {
            const auto plain = openWithDriver(tif, "GTiff");
            ASSERT_NE(plain, nullptr);
            const auto *rat = plain->GetRasterBand(1)->GetDefaultRAT();
            ASSERT_NE(rat, nullptr);
            ASSERT_GE(rat->GetRowCount(), 1);
            ASSERT_GE(rat->GetColumnCount(), 1);
            ASSERT_STREQ(rat->GetValueAsString(0, 0), "outside-data");
        }

        EXPECT_FALSE(gdalopen::validateSidecars(io::toUtf8(tif)).ok);
        EXPECT_EQ(openUserRaster(io::toUtf8(tif)), nullptr);
    }

    USER_OPEN_TEST(gdalOpenPolicy, attributeTableThatIsADbaseTableKeepsWorking)
    {
        if (!hasDriver("ESRI Shapefile"))
            GTEST_SKIP() << "No ESRI Shapefile driver";
        Scene s(TEST_NAME);
        const fs::path tif = writeGeoTiff(s.dataset / "a.tif");
        {
            const auto dbf = adoptDataset(GDALCreate(GDALGetDriverByName("ESRI Shapefile"),
                                                     io::toUtf8(s.dataset / "a.tif.vat.dbf").c_str(), 0, 0, 0,
                                                     GDT_Unknown, nullptr));
            ASSERT_NE(dbf, nullptr);
            auto *layer = dbf->CreateLayer("a.tif.vat", nullptr, wkbNone, nullptr);
            ASSERT_NE(layer, nullptr);
            OGRFieldDefn value("VALUE", OFTInteger);
            ASSERT_EQ(layer->CreateField(&value), OGRERR_NONE);
            OGRFeature feature(layer->GetLayerDefn());
            feature.SetField("VALUE", 7);
            ASSERT_EQ(layer->CreateFeature(&feature), OGRERR_NONE);
        }
        ASSERT_TRUE(fs::exists(s.dataset / "a.tif.vat.dbf"));

        const auto verdict = gdalopen::validateSidecars(io::toUtf8(tif));
        EXPECT_TRUE(verdict.ok) << verdict.reason;
        const auto dataset = openUserRaster(io::toUtf8(tif));
        ASSERT_NE(dataset, nullptr);
        const auto *rat = dataset->GetRasterBand(1)->GetDefaultRAT();
        ASSERT_NE(rat, nullptr);
        EXPECT_EQ(rat->GetValueAsInt(0, 0), 7);
    }

    // ---- What a VRT may hold -----------------------------------------------------------------

    // A VRT holds more than sources: a warped VRT deserialises its warp options while it
    // opens, and their transformer opens the datasets it names, any path or URL, with all the
    // drivers of GDAL: the geolocation arrays of a geolocation transformer (X_DATASET,
    // Y_DATASET) and the DEM of an RPC transformer (DEMPath). Only what a plain VRT reads in
    // place is accepted.

    // A warped VRT over a.tif, in its folder, whose warp uses `transformer`
    std::string warpedVrtOf(const std::string &transformer)
    {
        return std::string("<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\" subClass=\"VRTWarpedDataset\">\n"
                           "  <SRS>") +
               Srs +
               "</SRS>\n  <GeoTransform>576648.71, 0.05, 0, 5188209.785, 0, -0.05</GeoTransform>\n"
               "  <BlockXSize>32</BlockXSize>\n  <BlockYSize>32</BlockYSize>\n"
               "  <VRTRasterBand dataType=\"Float32\" band=\"1\" subClass=\"VRTWarpedRasterBand\" />\n"
               "  <GDALWarpOptions>\n    <WarpMemoryLimit>67108864</WarpMemoryLimit>\n"
               "    <ResampleAlg>NearestNeighbour</ResampleAlg>\n    <WorkingDataType>Float32</WorkingDataType>\n"
               "    <SourceDataset relativeToVRT=\"1\">a.tif</SourceDataset>\n"
               "    <Transformer>" +
               transformer +
               "</Transformer>\n    <BandList><BandMapping src=\"1\" dst=\"1\" /></BandList>\n"
               "  </GDALWarpOptions>\n</VRTDataset>\n";
    }

    // A geolocation transformer whose arrays are the raster `arrays`
    std::string geolocationTransformerOf(const std::string &arrays)
    {
        return "<GeoLocTransformer><Reversed>0</Reversed><Metadata>"
               "<MDI key=\"X_DATASET\">" +
               arrays + "</MDI><MDI key=\"X_BAND\">1</MDI><MDI key=\"Y_DATASET\">" + arrays +
               "</MDI><MDI key=\"Y_BAND\">1</MDI><MDI key=\"PIXEL_OFFSET\">0</MDI><MDI key=\"LINE_OFFSET\">0</MDI>"
               "<MDI key=\"PIXEL_STEP\">1</MDI><MDI key=\"LINE_STEP\">1</MDI></Metadata></GeoLocTransformer>";
    }

    // An RPC transformer that reads its heights from the raster `dem`
    std::string rpcTransformerOf(const std::string &dem)
    {
        const std::string zeros18 = " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
        return "<RPCTransformer><Reversed>0</Reversed><HeightOffset>0</HeightOffset><HeightScale>1</HeightScale>"
               "<DEMPath>" +
               dem +
               "</DEMPath><Metadata><MDI key=\"LINE_OFF\">16</MDI><MDI key=\"SAMP_OFF\">16</MDI>"
               "<MDI key=\"LAT_OFF\">45</MDI><MDI key=\"LONG_OFF\">10</MDI><MDI key=\"HEIGHT_OFF\">0</MDI>"
               "<MDI key=\"LINE_SCALE\">16</MDI><MDI key=\"SAMP_SCALE\">16</MDI><MDI key=\"LAT_SCALE\">0.01</MDI>"
               "<MDI key=\"LONG_SCALE\">0.01</MDI><MDI key=\"HEIGHT_SCALE\">100</MDI>"
               "<MDI key=\"LINE_NUM_COEFF\">0 0 -1" +
               zeros18 + "</MDI><MDI key=\"LINE_DEN_COEFF\">1 0" + zeros18 +
               "</MDI><MDI key=\"SAMP_NUM_COEFF\">0 1 0" + zeros18 + "</MDI><MDI key=\"SAMP_DEN_COEFF\">1 0" +
               zeros18 + "</MDI></Metadata></RPCTransformer>";
    }

    USER_OPEN_TEST(gdalOpenPolicy, vrtThatWarpsWithArraysOrADemOutsideIsRefused)
    {
        Scene s(TEST_NAME);
        writeGeoTiff(s.dataset / "a.tif");
        const fs::path vrt = s.dataset / "warped.vrt";
        const std::string outside = io::toGenericUtf8(s.outsideTif);

        for (const auto &transformer : {geolocationTransformerOf(outside), rpcTransformerOf(outside)})
        {
            SCOPED_TRACE(transformer);
            write(vrt, warpedVrtOf(transformer));
            EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
            EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr);
        }
    }

    // What a VRT names that GDAL would read, smuggled past a check of the elements it expects
    USER_OPEN_TEST(gdalOpenPolicy, vrtWithAnythingAPlainVrtDoesNotReadIsRefused)
    {
        Scene s(TEST_NAME);
        writeSecret(s.area.getFolder("outside") / "secret-constant.tif");
        writeGeoTiff(s.dataset / "a.tif");
        const fs::path vrt = s.dataset / "x.vrt";
        const std::string secret = io::toGenericUtf8(s.area.getFolder("outside") / "secret-constant.tif");
        const std::string plain = vrtOf("a.tif");
        const auto replacedIn = [](std::string v, const std::string &from, const std::string &to) {
            v.replace(v.find(from), from.size(), to);
            return v;
        };
        const auto replaced = [&](const std::string &from, const std::string &to) {
            return replacedIn(plain, from, to);
        };
        const std::string source = "<SourceFilename relativeToVRT=\"1\">a.tif</SourceFilename>";

        struct Case
        {
            const char *what;
            std::string xml;
        };
        const std::vector<Case> refused = {
            {"an element GDAL does not know", replaced("<SRS>", "<Unknown />\n  <SRS>")},
            {"an element GDAL does not know in a source", replaced("<SourceBand>", "<Unknown /><SourceBand>")},
            // GDAL finds the elements and attributes it reads whatever their case
            {"a source named in lower case",
             replaced(source, "<sourcefilename relativeToVRT=\"0\">" + secret + "</sourcefilename>")},
            {"a source named in an attribute",
             replacedIn(replaced(source, ""), "<ComplexSource>", "<ComplexSource SourceFilename=\"" + secret + "\">")},
            {"the dataset subClass given as an element",
             replaced("<SRS>", "<subClass>VRTWarpedDataset</subClass>\n  <SRS>")},
            {"a processed dataset", replaced("<VRTDataset ", "<VRTDataset subClass=\"VRTProcessedDataset\" ")},
            {"a raw band", replaced("band=\"1\"", "band=\"1\" subClass=\"VRTRawRasterBand\"")},
            {"open options of a source",
             replaced("<SourceBand>", "<OpenOptions><OOI key=\"GEOREF_SOURCES\">PAM</OOI></OpenOptions><SourceBand>")},
            {"a source that is an inline VRT",
             replaced(source, "<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\"><VRTRasterBand dataType=\"Float32\" "
                              "band=\"1\"><SimpleSource><SourceFilename relativeToVRT=\"0\">" +
                                  secret + "</SourceFilename></SimpleSource></VRTRasterBand></VRTDataset>")},
            {"a multidimensional group", replaced("<SRS>", "<Group name=\"/\" />\n  <SRS>")},
            {"metadata of another format", replaced("<SRS>", "<Metadata format=\"xml\"><x /></Metadata>\n  <SRS>")},
            {"the overviews named in the metadata",
             replaced("<SRS>", "<Metadata domain=\"OVERVIEWS\"><MDI key=\"OVERVIEW_FILE\">" + secret +
                                   "</MDI></Metadata>\n  <SRS>")},
            {"geolocation arrays named in the metadata",
             replaced("<SRS>", "<Metadata domain=\"geolocation\"><MDI key=\"X_DATASET\">" + secret +
                                   "</MDI></Metadata>\n  <SRS>")},
            {"a pixel function in a language", replaced("<NoDataValue>", "<PixelFunctionLanguage>Python"
                                                                         "</PixelFunctionLanguage><NoDataValue>")},
            {"a second document that is not a VRT", plain + "<OGRVRTDataSource />\n"},
        };
        for (const auto &c : refused)
        {
            SCOPED_TRACE(c.what);
            write(vrt, c.xml);
            const auto verdict = gdalopen::validateVrt(vrt, s.dataset);
            EXPECT_FALSE(verdict.ok);
            const auto dataset = openUserRaster(io::toUtf8(vrt));
            EXPECT_EQ(dataset, nullptr);
            if (dataset)
            {
                double value = 0;
                if (dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &value, 1, 1, GDT_Float64, 0, 0) ==
                    CE_None)
                {
                    EXPECT_NE(value, Secret);
                }
            }
        }
    }

    // The VRTs GDAL writes and what a plain VRT holds keep opening
    USER_OPEN_TEST(gdalOpenPolicy, vrtsThatGdalWritesKeepOpening)
    {
        Scene s(TEST_NAME);
        const fs::path a = writeGeoTiff(s.dataset / "tiles" / "a.tif");

        // gdalbuildvrt with a source nodata: complex sources with their properties
        const fs::path built = s.dataset / "built.vrt";
        {
            const char *args[] = {"-srcnodata", "0", nullptr};
            GDALBuildVRTOptions *options = GDALBuildVRTOptionsNew(const_cast<char **>(args), nullptr);
            ASSERT_NE(options, nullptr);
            const std::string source = io::toUtf8(a);
            const char *sources[] = {source.c_str()};
            const auto out = adoptDataset(
                GDALBuildVRT(io::toUtf8(built).c_str(), 1, nullptr, sources, options, nullptr));
            GDALBuildVRTOptionsFree(options);
            ASSERT_NE(out, nullptr);
        }

        // A copy as VRT: a simple source
        const fs::path copied = s.dataset / "copied.vrt";
        {
            const auto src = adoptDataset(GDALOpen(io::toUtf8(a).c_str(), GA_ReadOnly));
            ASSERT_NE(src, nullptr);
            const auto out = adoptDataset(GDALCreateCopy(GDALGetDriverByName("VRT"), io::toUtf8(copied).c_str(),
                                                         datasetHandle(src), FALSE, nullptr, nullptr, nullptr));
            ASSERT_NE(out, nullptr);
        }

        // Most of what a plain VRT reads, a derived band with a pixel function of GDAL among it
        const fs::path written = s.dataset / "written.vrt";
        write(written,
              std::string("<?xml version=\"1.0\"?>\n<!-- written by hand -->\n"
                          "<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\">\n  <SRS "
                          "dataAxisToSRSAxisMapping=\"1,2\">") +
                  Srs +
                  "</SRS>\n  <GeoTransform>576648.71, 0.05, 0, 5188209.785, 0, -0.05</GeoTransform>\n"
                  "  <Metadata><MDI key=\"AUTHOR\">someone</MDI></Metadata>\n"
                  "  <OverviewList resampling=\"average\">2 4</OverviewList>\n"
                  "  <VRTRasterBand dataType=\"Float32\" band=\"1\" subClass=\"VRTDerivedRasterBand\">\n"
                  "    <Description>height</Description>\n    <UnitType>m</UnitType>\n"
                  "    <ColorInterp>Gray</ColorInterp>\n    <NoDataValue>-9999</NoDataValue>\n"
                  "    <PixelFunctionType>sum</PixelFunctionType>\n"
                  "    <PixelFunctionArguments k=\"1\" />\n"
                  "    <SourceTransferType>Float64</SourceTransferType>\n"
                  "    <Overview><SourceFilename relativeToVRT=\"1\">tiles/a.tif</SourceFilename>"
                  "<SourceBand>1</SourceBand></Overview>\n"
                  "    <ComplexSource resampling=\"nearest\">\n"
                  "      <SourceFilename relativeToVRT=\"1\" shared=\"0\">tiles/a.tif</SourceFilename>\n"
                  "      <SourceBand>1</SourceBand>\n"
                  "      <SourceProperties RasterXSize=\"32\" RasterYSize=\"32\" DataType=\"Float32\" "
                  "BlockXSize=\"32\" BlockYSize=\"32\" />\n"
                  "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"32\" ySize=\"32\" />\n"
                  "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"32\" ySize=\"32\" />\n"
                  "      <NODATA>-9999</NODATA>\n      <ScaleOffset>0</ScaleOffset>\n"
                  "      <ScaleRatio>1</ScaleRatio>\n    </ComplexSource>\n  </VRTRasterBand>\n"
                  "  <MaskBand>\n    <VRTRasterBand dataType=\"Byte\">\n      <SimpleSource>\n"
                  "        <SourceFilename relativeToVRT=\"1\">tiles/a.tif</SourceFilename>\n"
                  "        <SourceBand>mask,1</SourceBand>\n      </SimpleSource>\n    </VRTRasterBand>\n"
                  "  </MaskBand>\n</VRTDataset>\n");

        for (const auto &vrt : {built, copied, written})
        {
            SCOPED_TRACE(io::toUtf8(vrt));
            const auto verdict = gdalopen::validateVrt(vrt, s.dataset);
            EXPECT_TRUE(verdict.ok) << verdict.reason;
            const auto dataset = openUserRaster(io::toUtf8(vrt));
            ASSERT_NE(dataset, nullptr);
            double value = 0;
            ASSERT_EQ(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &value, 1, 1, GDT_Float64, 0, 0),
                      CE_None);
            // The pixel function adds its k to the source
            EXPECT_EQ(value, vrt == written ? 101.0 : 100.0);
        }
    }

#ifndef _WIN32
    // The files a VRT names request nothing while it opens: neither the transformer of a
    // warped VRT, nor the overviews its metadata names, which GDAL opens as early as the open
    // of a VRT with an OverviewList
    USER_OPEN_TEST(gdalOpenPolicy, vrtRequestsNoUrlItsTransformerOrMetadataNames)
    {
        Scene s(TEST_NAME);
        writeGeoTiff(s.dataset / "a.tif");
        const fs::path vrt = s.dataset / "x.vrt";
        // A request that is made gives up soon instead of waiting for the silent port
        const ScopedThreadConfigOption timeout("GDAL_HTTP_TIMEOUT", "2");
        const ScopedThreadConfigOption noRetry("GDAL_HTTP_MAX_RETRY", "0");

        const auto urlOf = [](const SilentListener &listener) {
            return "/vsicurl/http://127.0.0.1:" + std::to_string(listener.port()) + "/remote.tif";
        };
        const std::vector<std::pair<const char *, std::function<std::string(const std::string &)>>> cases = {
            {"geolocation arrays", [](const std::string &url) { return warpedVrtOf(geolocationTransformerOf(url)); }},
            {"an RPC DEM", [](const std::string &url) { return warpedVrtOf(rpcTransformerOf(url)); }},
            {"overviews",
             [](const std::string &url) {
                 std::string v = vrtOf("a.tif");
                 v.replace(v.find("<SRS>"), 5,
                           "<Metadata domain=\"OVERVIEWS\"><MDI key=\"OVERVIEW_FILE\">" + url +
                               "</MDI></Metadata>\n  <OverviewList>2</OverviewList>\n  <SRS>");
                 return v;
             }},
        };
        for (const auto &[what, xml] : cases)
        {
            SCOPED_TRACE(what);
            SilentListener listener;
            write(vrt, xml(urlOf(listener)));
            EXPECT_FALSE(gdalopen::validateVrt(vrt, s.dataset).ok);
            EXPECT_EQ(openUserRaster(io::toUtf8(vrt)), nullptr);
            EXPECT_FALSE(listener.connected());
        }
    }

    // The GML driver reads the schemas the .xsd next to a GML includes with VSI, so a
    // /vsicurl/ schema is requested by curl itself, not through the HTTP callbacks GDAL offers
    USER_OPEN_TEST(gdalOpenPolicy, gmlRequestsNoSchemaThroughANetworkFileSystem)
    {
        if (!hasDriver("GML"))
            GTEST_SKIP() << "No GML driver";
        Scene s(TEST_NAME);
        SilentListener listener;
        const ScopedThreadConfigOption timeout("GDAL_HTTP_TIMEOUT", "2");
        const ScopedThreadConfigOption noRetry("GDAL_HTTP_MAX_RETRY", "0");

        const fs::path gml = s.dataset / "included.gml";
        write(gml, "<?xml version=\"1.0\"?>\n<ns:FeatureCollection xmlns:ns=\"http://example.com/ns\" "
                   "xmlns:gml=\"http://www.opengis.net/gml\">\n  <gml:featureMember><ns:t><ns:name>a</ns:name>"
                   "<ns:geom><gml:Point srsName=\"EPSG:4326\"><gml:coordinates>10,45</gml:coordinates></gml:Point>"
                   "</ns:geom></ns:t></gml:featureMember>\n</ns:FeatureCollection>\n");
        write(s.dataset / "included.xsd",
              "<?xml version=\"1.0\"?>\n<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
              "targetNamespace=\"http://example.com/ns\">\n  <xs:include schemaLocation=\"/vsicurl/http://127.0.0.1:" +
                  std::to_string(listener.port()) + "/schema.tif\"/>\n</xs:schema>\n");

        EXPECT_TRUE(openUserVector(io::toUtf8(gml)));
        EXPECT_FALSE(listener.connected());
    }

    // A remote raster libddb opens by its URL (a tile source, a thumbnail) still opens over the
    // network: the file being opened is the one file the network file systems open meanwhile
    USER_OPEN_TEST(gdalOpenPolicy, theFileBeingOpenedIsStillReadOverTheNetwork)
    {
        Scene s(TEST_NAME);
        NotFoundServer server;
        const ScopedThreadConfigOption noRetry("GDAL_HTTP_MAX_RETRY", "0");

        EXPECT_EQ(openUserRaster("/vsicurl/http://127.0.0.1:" + std::to_string(server.port()) + "/remote.tif"),
                  nullptr);
        const auto requests = server.requests();
        EXPECT_TRUE(std::any_of(requests.begin(), requests.end(),
                                [](const std::string &r) { return r.find(" /remote.tif ") != std::string::npos; }))
            << joined(requests);
    }
#endif
} // namespace
