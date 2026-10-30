/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The files libddb wrote itself (raster_artefacts.h): which places are artefacts and which files
// are not, what opens as one, and the tiler that reads nothing else: a raster of a user is
// converted by the raster helper into the artefacts folder of the cache, and a COG of a build is
// tiled as it is.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "build_layout.h"
#include "cog.h"
#include "ddb.h"
#include "exceptions.h"
#include "cpl_http.h"
#include "gdal_inc.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "gdaltiler.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_cache.h"
#include "raster_helper.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "tile_test_support.h"
#include "tilerhelper.h"
#include "userprofile.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    constexpr const char *kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    void writeFile(const fs::path &file, const std::string &content)
    {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::string readFile(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A small GeoTIFF in Web Mercator
    fs::path makeGeoTiff(const fs::path &file, int epsg = 3857)
    {
        fs::create_directories(file.parent_path());
        testutils::GeoRasterSpec spec;
        spec.width = 300;
        spec.height = 200;
        spec.bands = 3;
        spec.epsg = epsg;
        spec.originX = epsg == 3857 ? 1000000.0 : 500000.0;
        spec.originY = epsg == 3857 ? 5000000.0 : 5000000.0;
        spec.pixelSize = 2.0;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y * 3) % 256); };
        return testutils::createGeoRaster(file, spec);
    }

    // The cache of the test, apart from the one of the user
    class RasterArtefacts : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            area_ = std::make_unique<TestArea>(std::string("RasterArtefacts.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            cache_ = area_->getPath("cache");
            config_ = std::make_unique<ScopedProcessConfig>(json{{"cacheDir", io::toUtf8(cache_)}}.dump());
            dataset_ = area_->getPath("dataset");
            build_ = dataset_ / DDB_FOLDER / DDB_BUILD_PATH / kHash;
        }

        void TearDown() override
        {
            RasterCache::instance().clear();
        }

        std::unique_ptr<TestArea> area_;
        std::unique_ptr<ScopedProcessConfig> config_;
        fs::path cache_;
        fs::path dataset_;
        fs::path build_;
    };

    // ------------------------------------------------------------------ the places

    TEST_F(RasterArtefacts, theOutputsOfABuildThatGdalReadsAreArtefacts)
    {
        const fs::path cog = makeGeoTiff(build_ / CogBuildSubfolder / CogFileName);
        writeFile(build_ / VectorBuildSubfolder / VectorGpkgFileName, "gpkg");
        writeFile(build_ / MvtBuildSubfolder / "3" / "4" / "5.pbf", "tile");
        writeFile(build_ / MvtBuildSubfolder / MvtMetadataFileName, "{}");

        EXPECT_TRUE(raster::isLibddbArtefact(cog));
        EXPECT_TRUE(raster::isLibddbArtefact(build_ / VectorBuildSubfolder / VectorGpkgFileName));
        EXPECT_TRUE(raster::isLibddbArtefact(build_ / MvtBuildSubfolder / "3" / "4" / "5.pbf"));
        // The place is what counts: the file need not exist (a build about to write it)
        EXPECT_TRUE(raster::isLibddbArtefact(dataset_ / DDB_FOLDER / DDB_BUILD_PATH / kHash / CogBuildSubfolder / CogFileName));
        // A path with a dot segment is judged by where it leads
        EXPECT_TRUE(raster::isLibddbArtefact(build_ / "vec" / ".." / CogBuildSubfolder / CogFileName));
    }

    TEST_F(RasterArtefacts, otherFilesUnderTheBuildFolderAreNot)
    {
        makeGeoTiff(build_ / "foo.tif");
        makeGeoTiff(build_ / CogBuildSubfolder / "other.tif");
        writeFile(build_ / CogBuildSubfolder / (std::string(CogFileName) + ".stats.json"), "{}");
        writeFile(build_ / VectorBuildSubfolder / VectorLocalFileName, "{}");
        // The content of an extracted archive, and the outputs of the types GDAL does not read
        writeFile(build_ / Tiles3DBuildSubfolder / TilesetFileName, "{}");
        makeGeoTiff(build_ / Tiles3DBuildSubfolder / "texture.tif");
        // A name that is not a build hash, a folder that is not the build root, no build root at all
        makeGeoTiff(dataset_ / DDB_FOLDER / DDB_BUILD_PATH / "notahash" / CogBuildSubfolder / CogFileName);
        makeGeoTiff(dataset_ / DDB_FOLDER / "other" / kHash / CogBuildSubfolder / CogFileName);
        makeGeoTiff(dataset_ / CogBuildSubfolder / CogFileName);
        // The scratch folder of a build that is not finished
        makeGeoTiff(build_ / (std::string(CogBuildSubfolder) + TempFolderInfix + "x") / CogFileName);

        for (const auto &entry : fs::recursive_directory_iterator(dataset_))
            EXPECT_FALSE(raster::isLibddbArtefact(entry.path())) << entry.path();
        EXPECT_FALSE(raster::isLibddbArtefact(fs::path()));
        EXPECT_FALSE(raster::isLibddbArtefact(build_));
        EXPECT_FALSE(raster::isLibddbArtefact(build_ / CogBuildSubfolder));
    }

#ifndef _WIN32
    TEST_F(RasterArtefacts, aLinkIsFollowedBeforeThePlaceIsJudged)
    {
        const fs::path outside = makeGeoTiff(area_->getPath("outside") / "secret.tif");

        // A link in the place of the COG of a build, and one in the artefacts of the cache
        fs::create_directories(build_ / CogBuildSubfolder);
        fs::create_symlink(outside, build_ / CogBuildSubfolder / CogFileName);
        fs::create_symlink(outside, raster::artefactsDir() / "link.tif");
        // A link to a folder: what is below it is outside
        fs::create_directory_symlink(area_->getPath("outside"), raster::artefactsDir() / "folder");

        EXPECT_FALSE(raster::isLibddbArtefact(build_ / CogBuildSubfolder / CogFileName));
        EXPECT_FALSE(raster::isLibddbArtefact(raster::artefactsDir() / "link.tif"));
        EXPECT_FALSE(raster::isLibddbArtefact(raster::artefactsDir() / "folder" / "secret.tif"));
        EXPECT_THROW(raster::openArtefactRaster(raster::artefactsDir() / "link.tif"), GDALException);

        // A link from outside to a file that is one is judged by the file
        const fs::path real = makeGeoTiff(raster::artefactsDir() / "real.tif");
        fs::create_symlink(real, area_->getPath("outside") / "to-artefact.tif");
        EXPECT_TRUE(raster::isLibddbArtefact(area_->getPath("outside") / "to-artefact.tif"));
    }
#endif

    TEST_F(RasterArtefacts, theArtefactsFolderOfTheCacheHoldsArtefactsAndTheRestOfTheCacheDoesNot)
    {
        const fs::path artefacts = raster::artefactsDir();
        EXPECT_EQ(artefacts, cache_ / "artefacts");

        EXPECT_TRUE(raster::isLibddbArtefact(artefacts / "a.tif"));
        EXPECT_TRUE(raster::isLibddbArtefact(artefacts / "req-x" / "cog.tif"));
        EXPECT_FALSE(raster::isLibddbArtefact(artefacts)) << "the folder itself is not a file";

        // A download in the tiles folder is somebody else's file, whatever its name
        const fs::path download = makeGeoTiff(UserProfile::get()->getTilesDir() / "0123456789abcdef.tif");
        EXPECT_FALSE(raster::isLibddbArtefact(download));
        EXPECT_FALSE(raster::isLibddbArtefact(UserProfile::get()->getThumbsDir() / "a.webp"));
        EXPECT_FALSE(raster::isLibddbArtefact(artefacts / ".." / "tiles" / "0123456789abcdef.tif"));
        EXPECT_FALSE(raster::isLibddbArtefact(cache_ / "artefacts-not" / "a.tif"));
        EXPECT_FALSE(raster::isLibddbArtefact(area_->getPath("elsewhere.tif")));
    }

    // ------------------------------------------------------------------ opening

    TEST_F(RasterArtefacts, aFileThatIsNotAnArtefactIsNotOpened)
    {
        const fs::path raw = makeGeoTiff(area_->getPath("raw.tif"));
        try
        {
            raster::openArtefactRaster(raw);
            FAIL() << "no exception";
        }
        catch (const GDALException &e)
        {
            EXPECT_NE(std::string(e.what()).find("not a file libddb wrote"), std::string::npos) << e.what();
        }
        EXPECT_THROW(raster::openArtefactVector(raw), GDALException);

        // The same file in the place of the artefacts opens
        const fs::path copy = raster::artefactsDir() / "raw.tif";
        fs::copy_file(raw, copy);
        const auto dataset = raster::openArtefactRaster(copy);
        ASSERT_TRUE(dataset);
        EXPECT_EQ(GDALGetRasterXSize(datasetHandle(dataset)), 300);
    }

    TEST_F(RasterArtefacts, onlyTheDriversOfTheArtefactsOpenAnArtefact)
    {
        const fs::path real = makeGeoTiff(raster::artefactsDir() / "real.tif");

        // A VRT is a list of files to read, wherever it is and whatever it is named
        const std::string vrt = "<VRTDataset rasterXSize=\"300\" rasterYSize=\"200\"><VRTRasterBand dataType=\"Byte\" band=\"1\">"
                                "<SimpleSource><SourceFilename relativeToVRT=\"0\">" +
                                io::toUtf8(real) +
                                "</SourceFilename><SourceBand>1</SourceBand></SimpleSource></VRTRasterBand></VRTDataset>";
        writeFile(raster::artefactsDir() / "list.vrt", vrt);
        writeFile(raster::artefactsDir() / "disguised.tif", vrt);
        EXPECT_FALSE(raster::openArtefactRaster(raster::artefactsDir() / "list.vrt"));
        EXPECT_FALSE(raster::openArtefactRaster(raster::artefactsDir() / "disguised.tif"));

        // A PNG with the name of a GeoTIFF is no GeoTIFF
        const fs::path png = raster::artefactsDir() / "image.tif";
        GDALDatasetH source = GDALOpen(io::toUtf8(real).c_str(), GA_ReadOnly);
        ASSERT_NE(source, nullptr);
        GDALClose(GDALCreateCopy(GDALGetDriverByName("PNG"), io::toUtf8(png).c_str(), source, FALSE, nullptr, nullptr, nullptr));
        GDALClose(source);
        ASSERT_TRUE(fs::exists(png));
        EXPECT_FALSE(raster::openArtefactRaster(png));

        // A GeoTIFF is not a vector, and a raster open does not take a GeoPackage
        EXPECT_FALSE(raster::openArtefactVector(real));
        const fs::path gpkg = raster::artefactsDir() / "layers.gpkg";
        {
            GDALDatasetH dataset = GDALCreate(GDALGetDriverByName("GPKG"), io::toUtf8(gpkg).c_str(), 0, 0, 0, GDT_Unknown, nullptr);
            ASSERT_NE(dataset, nullptr);
            GDALDatasetCreateLayer(dataset, "points", nullptr, wkbPoint, nullptr);
            GDALClose(dataset);
        }
        EXPECT_FALSE(raster::openArtefactRaster(gpkg));
        const auto layers = raster::openArtefactVector(gpkg);
        ASSERT_TRUE(layers);
        EXPECT_EQ(GDALDatasetGetLayerCount(datasetHandle(layers)), 1);

        EXPECT_TRUE(raster::openArtefactRaster(real));
    }

    TEST_F(RasterArtefacts, aRasterWhoseMetadataNamesAnotherDatasetIsNotOpened)
    {
        // The metadata GDAL opens the named dataset from (the overviews of a raster, the arrays of
        // a geolocation) came from a file of a user if it is in a copy: refused, as for a user file
        const fs::path elsewhere = makeGeoTiff(area_->getPath("elsewhere.tif"));
        const std::pair<const char *, const char *> items[] = {
            {"OVERVIEWS", "OVERVIEW_FILE"}, {"GEOLOCATION", "X_DATASET"}, {"GEOLOCATION", "Y_DATASET"}};
        for (const auto &[domain, item] : items)
        {
            SCOPED_TRACE(item);
            const fs::path artefact = makeGeoTiff(raster::artefactsDir() / (std::string(item) + ".tif"));
            {
                auto update = adoptDataset(GDALOpenEx(io::toUtf8(artefact).c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                                                      nullptr, nullptr, nullptr));
                ASSERT_TRUE(update);
                ASSERT_EQ(GDALSetMetadataItem(datasetHandle(update), item, io::toUtf8(elsewhere).c_str(), domain),
                          CE_None);
            }
            EXPECT_FALSE(raster::openArtefactRaster(artefact));
            EXPECT_FALSE(raster::reopenWrittenRaster(artefact));
        }
        EXPECT_TRUE(raster::openArtefactRaster(makeGeoTiff(raster::artefactsDir() / "plain.tif")));
    }

    TEST_F(RasterArtefacts, theGuardOfTheOpenPolicyIsTheOneAnArtefactOpensWith)
    {
        const fs::path file = makeGeoTiff(raster::artefactsDir() / "guarded.tif");
        EXPECT_EQ(CPLGetThreadLocalConfigOption("CPL_VSIL_CURL_ALLOWED_FILENAME", nullptr), nullptr);
        {
            const gdalopen::NoNetworkWhileOpening guard(io::toUtf8(file));
            EXPECT_STREQ(CPLGetThreadLocalConfigOption("CPL_VSIL_CURL_ALLOWED_FILENAME", nullptr),
                         io::toUtf8(file).c_str());
            // A request is refused without reaching anything
            CPLHTTPResult *result = CPLHTTPFetch("http://127.0.0.1:9/", nullptr);
            ASSERT_NE(result, nullptr);
            EXPECT_NE(result->nStatus, 0);
            ASSERT_NE(result->pszErrBuf, nullptr);
            EXPECT_NE(std::string(result->pszErrBuf).find("no request is made"), std::string::npos) << result->pszErrBuf;
            CPLHTTPDestroyResult(result);
        }
        EXPECT_EQ(CPLGetThreadLocalConfigOption("CPL_VSIL_CURL_ALLOWED_FILENAME", nullptr), nullptr);
    }

    TEST_F(RasterArtefacts, aFileOfTheMemoryFileSystemIsAnArtefactAndOpensAsAGeoPackage)
    {
        EXPECT_TRUE(raster::isLibddbArtefact("/vsimem/ddb-abc.gpkg"));
        EXPECT_FALSE(raster::isLibddbArtefact("/vsimem/../etc/passwd"));
        EXPECT_FALSE(raster::isLibddbArtefact("/vsimemory/a.gpkg"));
        EXPECT_FALSE(raster::isLibddbArtefact("/vsicurl/http://example.com/a.tif"));

        const VsiMemFile memory = VsiMemFile::unique("artefact-test-", ".gpkg");
        {
            GDALDatasetH dataset = GDALCreate(GDALGetDriverByName("GPKG"), memory.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
            ASSERT_NE(dataset, nullptr);
            GDALDatasetCreateLayer(dataset, "points", nullptr, wkbPoint, nullptr);
            GDALClose(dataset);
        }
        const auto layers = raster::openArtefactVector(io::fromUtf8(memory.path()));
        ASSERT_TRUE(layers);
        EXPECT_EQ(GDALDatasetGetLayerCount(datasetHandle(layers)), 1);
    }

    TEST_F(RasterArtefacts, aLinkedFolderInThePlaceOfARoleFolderIsNotAnArtefact)
    {
#ifdef _WIN32
        GTEST_SKIP() << "creating a link needs a privilege on Windows";
#else
        // The folder of the COG of a build is a link to a folder somebody else wrote
        const fs::path outside = area_->getPath("outside");
        makeGeoTiff(outside / CogFileName);
        fs::create_directories(build_);
        fs::create_directory_symlink(outside, build_ / CogBuildSubfolder);
        EXPECT_FALSE(raster::isLibddbArtefact(build_ / CogBuildSubfolder / CogFileName));
        EXPECT_THROW(raster::openArtefactRaster(build_ / CogBuildSubfolder / CogFileName), GDALException);

        // The build folder itself, and the dataset above it
        const fs::path real = makeGeoTiff(dataset_ / "real-build" / CogBuildSubfolder / CogFileName);
        fs::create_directory_symlink(dataset_ / "real-build", dataset_ / DDB_FOLDER / DDB_BUILD_PATH / "linked");
        EXPECT_FALSE(raster::isLibddbArtefact(dataset_ / DDB_FOLDER / DDB_BUILD_PATH / "linked" / CogBuildSubfolder / CogFileName));
        EXPECT_FALSE(raster::isLibddbArtefact(real));
#endif
    }

    TEST_F(RasterArtefacts, aPathThatClimbsOutOfTheBuildFolderIsJudgedByWhereItLeads)
    {
        const fs::path planted = makeGeoTiff(area_->getPath("outside") / "x" / CogBuildSubfolder / CogFileName);
        // The role path of a build, reached from another build by dot segments: that build's
        EXPECT_TRUE(raster::isLibddbArtefact(build_ / ".." / kHash / CogBuildSubfolder / CogFileName));
        // Out of the build root into a folder that only looks like one
        EXPECT_FALSE(raster::isLibddbArtefact(build_ / ".." / ".." / ".." / ".." / "outside" / "x" / CogBuildSubfolder / CogFileName));
        EXPECT_FALSE(raster::isLibddbArtefact(build_ / CogBuildSubfolder / ".." / ".." / ".." / ".." / ".." / "outside" / "x" / CogBuildSubfolder / CogFileName));
        EXPECT_FALSE(raster::isLibddbArtefact(planted));
        EXPECT_THROW(raster::openArtefactRaster(planted), GDALException);
    }

    TEST_F(RasterArtefacts, anArtefactOpensForUpdateAndAFileJustWrittenReopensWhereverItIs)
    {
        const fs::path artefact = makeGeoTiff(raster::artefactsDir() / "update.tif");
        {
            const auto update = raster::openArtefactRaster(artefact, raster::ArtefactAccess::Update);
            ASSERT_TRUE(update);
            GDALRasterBandH band = GDALGetRasterBand(datasetHandle(update), 1);
            EXPECT_EQ(GDALSetRasterNoDataValue(band, 7.0), CE_None);
        }
        {
            const auto read = raster::openArtefactRaster(artefact);
            ASSERT_TRUE(read);
            int hasNoData = 0;
            EXPECT_EQ(GDALGetRasterNoDataValue(GDALGetRasterBand(datasetHandle(read), 1), &hasNoData), 7.0);
            EXPECT_TRUE(hasNoData);
        }

        // Not an artefact by place, and not refused for it: it is what the caller wrote just now
        const fs::path written = makeGeoTiff(area_->getPath("export.tif"));
        EXPECT_THROW(raster::openArtefactRaster(written, raster::ArtefactAccess::Update), GDALException);
        EXPECT_TRUE(raster::reopenWrittenRaster(written, raster::ArtefactAccess::Update));
        EXPECT_TRUE(raster::reopenWrittenRaster(written));

        // Only a regular file, and with the drivers of the artefacts
        EXPECT_FALSE(raster::reopenWrittenRaster(area_->getPath("missing.tif")));
        EXPECT_FALSE(raster::reopenWrittenRaster(area_->getPath("outside")));
        writeFile(area_->getPath("list.vrt"), "<VRTDataset rasterXSize=\"1\" rasterYSize=\"1\"/>");
        EXPECT_FALSE(raster::reopenWrittenRaster(area_->getPath("list.vrt")));
#ifndef _WIN32
        fs::create_symlink(written, area_->getPath("link.tif"));
        EXPECT_FALSE(raster::reopenWrittenRaster(area_->getPath("link.tif")));
#endif
    }

    // ------------------------------------------------------------------ names and folders

    TEST_F(RasterArtefacts, theKeyOfAFileChangesWithTheFileAndOnlyThen)
    {
        const fs::path file = area_->getPath("source.tif");
        writeFile(file, "one");
        const std::string key = raster::artefactKey(file);
        EXPECT_EQ(key, raster::artefactKey(file));
        EXPECT_EQ(key.find_first_not_of("0123456789abcdefABCDEF"), std::string::npos) << key;

        writeFile(file, "one and more");
        EXPECT_NE(raster::artefactKey(file), key) << "another size is another file";

        const fs::path other = area_->getPath("other.tif");
        writeFile(other, "one and more");
        fs::last_write_time(other, fs::last_write_time(file));
        EXPECT_NE(raster::artefactKey(other), raster::artefactKey(file)) << "another path is another file";

        fs::last_write_time(file, fs::last_write_time(file) - std::chrono::hours(1));
        EXPECT_NE(raster::artefactKey(file), raster::artefactKey(other));
        EXPECT_THROW(raster::artefactKey(area_->getPath("missing.tif")), std::exception);
    }

    TEST_F(RasterArtefacts, aRequestFolderIsPrivateInTheArtefactsAndGoneWithItsOwner)
    {
        fs::path folder;
        {
            const io::ScopedTempDir request = raster::makeArtefactRequestFolder("test-");
            folder = request.path();
            EXPECT_EQ(folder.parent_path(), raster::artefactsDir());
            EXPECT_TRUE(raster::isLibddbArtefact(folder / "out.tif"));
#ifndef _WIN32
            EXPECT_EQ(fs::status(folder).permissions() & fs::perms::all, fs::perms::owner_all);
#endif
            EXPECT_TRUE(fs::is_directory(folder));
        }
        EXPECT_FALSE(fs::exists(folder));
    }

    TEST_F(RasterArtefacts, publishingReplacesTheTargetWithTheWholeFile)
    {
        const fs::path target = raster::artefactsDir() / "target.tif";
        writeFile(target, "old");
        const io::ScopedTempDir request = raster::makeArtefactRequestFolder("publish-");
        writeFile(request.path() / "new.tif", "new content");

        raster::publishArtefact(request.path() / "new.tif", target);

        EXPECT_EQ(readFile(target), "new content");
        EXPECT_FALSE(fs::exists(request.path() / "new.tif"));
        EXPECT_THROW(raster::publishArtefact(request.path() / "missing.tif", target), FSException);
        EXPECT_EQ(readFile(target), "new content") << "a failed publish leaves the target as it was";
    }

    TEST_F(RasterArtefacts, theCleanupRemovesWhatWasNotTouchedForFiveDaysAndNothingElse)
    {
        const fs::path artefacts = raster::artefactsDir();
        const auto longAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 6);
        writeFile(artefacts / "old.tif", "old");
        fs::last_write_time(artefacts / "old.tif", longAgo);
        writeFile(artefacts / "req-old" / "cog.tif", "left behind by a request");
        fs::last_write_time(artefacts / "req-old", longAgo);
        writeFile(artefacts / "recent.tif", "recent");
        fs::last_write_time(artefacts / "recent.tif", fs::file_time_type::clock::now() - std::chrono::hours(24 * 4));

        TilerHelper::cleanupUserCache();

        EXPECT_FALSE(fs::exists(artefacts / "old.tif"));
        EXPECT_FALSE(fs::exists(artefacts / "req-old"));
        EXPECT_TRUE(fs::exists(artefacts / "recent.tif"));
    }

    // ------------------------------------------------------------------ the tiler

    // Stands in for the raster helper: a file of a user is whatever the test names, the answer to a
    // probe is a GeoRaster, and the COG is a prepared one that is copied to where the host asks.
    // Every request is recorded. Nothing reads the file of the user, so the tiler cannot have
    // made its tiles from it unless it opened it itself.
    class ConvertingHelper : public raster::IRasterHelper
    {
      public:
        explicit ConvertingHelper(fs::path cog) : cog_(std::move(cog)) {}

        raster::ProbeResult probe(const fs::path &path, const raster::ProbeParams &) override
        {
            record("probe", path);
            raster::ProbeResult result;
            result.type = "GeoRaster";
            return result;
        }
        void thumbnail(const fs::path &, const fs::path &, const raster::ThumbnailParams &) override
        {
            ADD_FAILURE() << "no thumbnail is asked for";
        }
        void normalize(const fs::path &, const fs::path &, const json &) override
        {
            ADD_FAILURE() << "no normalization is asked for";
        }
        void geoproject(const fs::path &, const fs::path &, const json &) override
        {
            ADD_FAILURE() << "no photo is projected";
        }
        void buildCog(const fs::path &path, const fs::path &out, const json &params, const raster::ProgressCallback &) override
        {
            record("cog", path);
            lastCogOut = out;
            lastCogParams = params;
            fs::copy_file(cog_, out, fs::copy_options::overwrite_existing);
        }
        void buildVector(const fs::path &, const fs::path &, const json &, const raster::ProgressCallback &) override
        {
            ADD_FAILURE() << "no vector is built";
        }

        size_t count(const std::string &op) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t n = 0;
            for (const auto &[name, path] : requests_)
                n += name == op;
            return n;
        }
        fs::path pathOf(const std::string &op) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &[name, path] : requests_)
                if (name == op)
                    return path;
            return {};
        }

        fs::path lastCogOut;
        json lastCogParams;

      private:
        void record(const std::string &op, const fs::path &path)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.emplace_back(op, path);
        }

        fs::path cog_;
        mutable std::mutex mutex_;
        std::vector<std::pair<std::string, fs::path>> requests_;
    };

    // The tile at the deepest zoom of the middle of the raster, from the artefact
    TileRequest middleTile(const fs::path &source, const fs::path &artefact)
    {
        GDALTiler tiler(io::toUtf8(artefact), "", 256);
        const int z = tiler.getMinMaxZ().max;
        const auto bounds = tiler.getMinMaxCoordsForZ(z);
        return testTile(source, z, (bounds.min.x + bounds.max.x) / 2, (bounds.min.y + bounds.max.y) / 2, 256, false, false);
    }

    std::vector<uint8_t> memoryTile(TileRequest request)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        request.outBuffer = &buffer;
        request.outBufferSize = &size;
        TilerHelper::getTile(request);
        return testutils::takeBuffer(buffer, size);
    }

    TEST_F(RasterArtefacts, aRawRasterIsTiledFromACogOfTheCacheAndNeverOpenedByTheTiler)
    {
        const fs::path preparedCog = makeGeoTiff(area_->getPath("prepared") / "cog.tif");
        // A file GDAL cannot open: only the helper (here, the stand-in) converts it
        const fs::path raw = area_->getPath("raw.tif");
        writeFile(raw, "bytes that no driver of GDAL opens");
        ConvertingHelper helper(preparedCog);
        const raster::ScopedRasterHelper installed(helper);

        const fs::path cog = TilerHelper::toGeoTIFF(raw);

        EXPECT_EQ(cog.parent_path(), raster::artefactsDir());
        EXPECT_EQ(cog.filename(), fs::path(raster::artefactKey(raw) + ".tif"));
        EXPECT_TRUE(raster::isLibddbArtefact(cog));
        EXPECT_EQ(helper.count("cog"), 1u);
        EXPECT_EQ(helper.pathOf("cog"), raw);
        // The helper wrote into a folder of the artefacts, and the file was moved to its name
        EXPECT_EQ(helper.lastCogOut.parent_path().parent_path(), raster::artefactsDir());
        EXPECT_FALSE(fs::exists(helper.lastCogOut));
        EXPECT_TRUE(helper.lastCogParams.contains("threads"));

        // The tiles come from the COG: the tiler of the raw file refuses it, the helper's COG serves
        EXPECT_THROW(GDALTiler(io::toUtf8(raw), "", 256), GDALException);
        const std::vector<uint8_t> tile = memoryTile(middleTile(raw, cog));
        EXPECT_FALSE(tile.empty());
        EXPECT_EQ(tile, memoryTile(middleTile(raw, cog)));
        // Once per state of the file
        EXPECT_EQ(helper.count("cog"), 1u);
        EXPECT_EQ(cog, TilerHelper::toGeoTIFF(raw));
        EXPECT_EQ(helper.count("cog"), 1u);

        // A forced tile is converted again; a changed file is another artefact
        EXPECT_EQ(cog, TilerHelper::toGeoTIFF(raw, true));
        EXPECT_EQ(helper.count("cog"), 2u);
        writeFile(raw, "other bytes, other size, that no driver of GDAL opens");
        EXPECT_NE(cog, TilerHelper::toGeoTIFF(raw));
        EXPECT_EQ(helper.count("cog"), 3u);
    }

    TEST_F(RasterArtefacts, theTilesOfARawRasterAreTheTilesOfItsCog)
    {
        // The real program: the COG it builds is the one the tiles come from
        const fs::path raw = makeGeoTiff(area_->getPath("raw.tif"), 32632);

        const fs::path cog = TilerHelper::toGeoTIFF(raw);
        ASSERT_TRUE(fs::exists(cog));
        EXPECT_TRUE(raster::isLibddbArtefact(cog));

        const TileRequest request = middleTile(raw, cog);
        EXPECT_EQ(memoryTile(request), memoryTile(middleTile(cog, cog)))
            << "the tiles of the raw file and of the COG are the same";
    }

    TEST_F(RasterArtefacts, aCogThatServesTilesIsTouchedAndTheCleanupLeavesIt)
    {
        const fs::path preparedCog = makeGeoTiff(area_->getPath("prepared") / "cog.tif");
        ConvertingHelper helper(preparedCog);
        const raster::ScopedRasterHelper installed(helper);
        const fs::path raw = area_->getPath("raw.tif");
        writeFile(raw, "bytes that no driver of GDAL opens");

        const fs::path cog = TilerHelper::toGeoTIFF(raw);
        const fs::path unused = raster::artefactsDir() / "unused.tif";
        writeFile(unused, "not used for days");
        const auto longAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 6);
        fs::last_write_time(cog, longAgo);
        fs::last_write_time(unused, longAgo);

        EXPECT_EQ(TilerHelper::toGeoTIFF(raw), cog) << "served from the cache";
        TilerHelper::cleanupUserCache();

        EXPECT_TRUE(fs::exists(cog)) << "a COG that tiles are made from stays";
        EXPECT_FALSE(fs::exists(unused));
        EXPECT_EQ(helper.count("cog"), 1u);
    }

    TEST_F(RasterArtefacts, aPhotoIsGeoprojectedIntoTheArtefactsForTheTiler)
    {
        const fs::path photo = area_->downloadTestAsset(testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");

        const fs::path projected = TilerHelper::toGeoTIFF(photo);

        EXPECT_EQ(projected.parent_path(), raster::artefactsDir());
        EXPECT_EQ(projected.filename(), fs::path(raster::artefactKey(photo) + "-geoprojected.tif"));
        const auto dataset = raster::openArtefactRaster(projected);
        ASSERT_TRUE(dataset);
        EXPECT_EQ(GDALGetRasterCount(datasetHandle(dataset)), 4) << "RGB and alpha";
        EXPECT_FALSE(memoryTile(middleTile(photo, projected)).empty());
        EXPECT_EQ(projected, TilerHelper::toGeoTIFF(photo)) << "made once";
        for (const auto &entry : fs::directory_iterator(raster::artefactsDir()))
            EXPECT_EQ(entry.path().filename().string().rfind("req-", 0), std::string::npos)
                << "the folder of the request is gone: " << entry.path();
    }

    TEST_F(RasterArtefacts, aCogOfABuildIsTiledAsItIs)
    {
        const fs::path built = makeGeoTiff(build_ / CogBuildSubfolder / CogFileName);
        ConvertingHelper helper(built);
        const raster::ScopedRasterHelper installed(helper);

        EXPECT_EQ(TilerHelper::toGeoTIFF(built), built);
        EXPECT_FALSE(memoryTile(middleTile(built, built)).empty());
        EXPECT_EQ(helper.count("cog"), 0u) << "a file libddb wrote is not converted";
        EXPECT_EQ(helper.count("probe"), 0u);
        EXPECT_TRUE(fs::is_empty(raster::artefactsDir()));
    }

    TEST_F(RasterArtefacts, aConversionThatFailsLeavesNothingInTheCache)
    {
        ConvertingHelper helper(area_->getPath("missing.tif"));
        const raster::ScopedRasterHelper installed(helper);
        const fs::path raw = area_->getPath("raw.tif");
        writeFile(raw, "bytes");

        // The stand-in cannot write its COG: nothing is left behind in the cache
        EXPECT_THROW(TilerHelper::toGeoTIFF(raw), std::exception);
        for (const auto &entry : fs::directory_iterator(raster::artefactsDir()))
            ADD_FAILURE() << "left in the artefacts: " << entry.path();
    }

    TEST_F(RasterArtefacts, aFileThatIsNoCogIsNotPublished)
    {
        const fs::path notACog = area_->getPath("prepared") / "cog.tif";
        writeFile(notACog, "this is not a GeoTIFF");
        ConvertingHelper helper(notACog);
        const raster::ScopedRasterHelper installed(helper);
        const fs::path raw = area_->getPath("raw.tif");
        writeFile(raw, "bytes");

        EXPECT_THROW(TilerHelper::toGeoTIFF(raw), GDALException);
        for (const auto &entry : fs::directory_iterator(raster::artefactsDir()))
            ADD_FAILURE() << "left in the artefacts: " << entry.path();
    }

    TEST_F(RasterArtefacts, aRemoteRasterIsDownloadedAndThenConverted)
    {
        const std::string url = testDataUrl("ortho/edge/utm.tif");
        const fs::path preparedCog = makeGeoTiff(area_->getPath("prepared") / "cog.tif");
        ConvertingHelper helper(preparedCog);
        const raster::ScopedRasterHelper installed(helper);

        const fs::path cog = TilerHelper::toGeoTIFF(url);

        // The download is a file of somebody else, kept in the tiles of the cache, and it is what
        // the helper converts; the artefact is the COG
        const fs::path converted = helper.pathOf("cog");
        EXPECT_EQ(converted.parent_path(), UserProfile::get()->getTilesDir());
        EXPECT_FALSE(raster::isLibddbArtefact(converted));
        EXPECT_TRUE(raster::isLibddbArtefact(cog));
        EXPECT_EQ(cog.filename(), fs::path(raster::artefactKey(converted) + ".tif"));
    }

    // ------------------------------------------------------------------ the COG of an output

    // The stand-in that also writes the statistics next to the COG, as the helper does, and notes
    // the access of the folder it was told to write in
    class StatsWritingHelper : public ConvertingHelper
    {
      public:
        using ConvertingHelper::ConvertingHelper;

        void buildCog(const fs::path &path, const fs::path &out, const json &params,
                      const raster::ProgressCallback &progress) override
        {
            outFolderPerms = fs::status(out.parent_path()).permissions();
            ConvertingHelper::buildCog(path, out, params, progress);
            writeFile(statsSidecarOf(out), R"({"bands":{}})");
        }

        fs::perms outFolderPerms = fs::perms::unknown;
    };

    // The folder of an output is the caller's and holds other files (on Windows, a folder the
    // confined helper may write in gets a label that the files already in it take as well): the
    // helper writes the COG and its statistics into a private folder made beside the output,
    // and they are moved to their names
    TEST_F(RasterArtefacts, aCogIsBuiltInAPrivateFolderBesideItsOutputAndMovedToItsName)
    {
        StatsWritingHelper helper(makeGeoTiff(area_->getPath("prepared") / "cog.tif"));
        const raster::ScopedRasterHelper installed(helper);
        const fs::path folder = area_->getFolder("outputs");
        const fs::path other = folder / "program.exe";
        writeFile(other, "a file of the user");
        const fs::path input = area_->getPath("raw.tif");
        writeFile(input, "bytes");
        const fs::path out = folder / "x.tif";

        buildCog(io::toUtf8(input), io::toUtf8(out));

        ASSERT_EQ(helper.count("cog"), 1u);
        EXPECT_EQ(helper.pathOf("cog"), input);
        const fs::path staging = helper.lastCogOut.parent_path();
        EXPECT_NE(staging, folder) << "the helper never writes into the folder of the output";
        EXPECT_EQ(staging.parent_path(), folder);
#ifndef _WIN32
        EXPECT_EQ(helper.outFolderPerms, fs::perms::owner_all) << "a folder only this user enters";
#endif
        EXPECT_FALSE(fs::exists(staging)) << "the private folder is removed";

        EXPECT_TRUE(raster::reopenWrittenRaster(out));
        EXPECT_EQ(readFile(statsSidecarOf(out)), R"({"bands":{}})");
        std::vector<fs::path> entries;
        for (const auto &entry : fs::directory_iterator(folder))
            entries.push_back(entry.path());
        std::sort(entries.begin(), entries.end());
        EXPECT_EQ(entries, (std::vector<fs::path>{other, out, statsSidecarOf(out)}));
    }

    // A COG built again without statistics leaves none of the COG it replaced next to it
    TEST_F(RasterArtefacts, aCogBuiltWithoutStatisticsRemovesTheStatisticsOfTheCogItReplaces)
    {
        ConvertingHelper helper(makeGeoTiff(area_->getPath("prepared") / "cog.tif"));
        const raster::ScopedRasterHelper installed(helper);
        const fs::path out = area_->getFolder("outputs") / "x.tif";
        writeFile(out, "the COG of before");
        writeFile(statsSidecarOf(out), "the statistics of the COG of before");
        const fs::path input = area_->getPath("raw.tif");
        writeFile(input, "bytes");

        buildCog(io::toUtf8(input), io::toUtf8(out));

        EXPECT_TRUE(raster::reopenWrittenRaster(out));
        EXPECT_FALSE(fs::exists(statsSidecarOf(out)));
    }

    // A COG the helper wrote that does not open is not moved to its name, and its folder goes
    TEST_F(RasterArtefacts, aCogThatDoesNotOpenIsNotMovedToTheOutput)
    {
        const fs::path notACog = area_->getPath("prepared") / "cog.tif";
        writeFile(notACog, "this is not a GeoTIFF");
        ConvertingHelper helper(notACog);
        const raster::ScopedRasterHelper installed(helper);
        const fs::path folder = area_->getFolder("outputs");
        const fs::path input = area_->getPath("raw.tif");
        writeFile(input, "bytes");

        EXPECT_THROW(buildCog(io::toUtf8(input), io::toUtf8(folder / "x.tif")), GDALException);
        EXPECT_TRUE(fs::is_empty(folder));
    }
}
