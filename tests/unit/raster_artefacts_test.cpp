/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The files libddb wrote itself (raster_artefacts.h): which places are artefacts and which files
// are not, what opens as one, and the tiler that reads nothing else: a raster of a user is
// converted by the raster helper into the artefacts folder of the cache, and a COG of a build is
// tiled as it is. A photo is tiled from itself and leaves nothing in the cache.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "open_descriptors.h"
#include "build_layout.h"
#include "cog.h"
#include "ddb.h"
#include "exceptions.h"
#include "cpl_http.h"
#include "gdal_inc.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "photo_tile.h"
#include "photo_tiler.h"
#include "raster_tiler.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "geoproject.h"
#include "raster_artefacts.h"
#include "raster_cache.h"
#include "raster_helper.h"
#include "raster_stats.h"
#include "raster_test_utils.h"
#include "tilers_raster.h"
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
        EXPECT_THROW(raster::requireArtefact(raster::artefactsDir() / "link.tif"), GDALException);

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
            raster::requireArtefact(raw);
            FAIL() << "no exception";
        }
        catch (const GDALException &e)
        {
            EXPECT_NE(std::string(e.what()).find("not a file libddb wrote"), std::string::npos) << e.what();
        }
        EXPECT_THROW(raster::openArtefactVector(raw), GDALException);

        // The same file in the place of the artefacts is one, and the host reads it as it is
        const fs::path copy = raster::artefactsDir() / "raw.tif";
        fs::copy_file(raw, copy);
        EXPECT_NO_THROW(raster::requireArtefact(copy));
        const auto raster = tilers::Raster::openInput(io::toUtf8(copy));
        ASSERT_TRUE(raster);
        EXPECT_EQ(raster->file(), copy);
        EXPECT_EQ(raster->width(), 300);
    }

    // The host reads an artefact as a TIFF and nothing else: a VRT (a list of other files), a PNG or a
    // GeoPackage in the place of an artefact is no raster to it
    TEST_F(RasterArtefacts, anArtefactThatIsNoTiffIsNoRaster)
    {
        const fs::path real = makeGeoTiff(raster::artefactsDir() / "real.tif");

        // A VRT is a list of files to read, wherever it is and whatever it is named
        const std::string vrt = "<VRTDataset rasterXSize=\"300\" rasterYSize=\"200\"><VRTRasterBand dataType=\"Byte\" band=\"1\">"
                                "<SimpleSource><SourceFilename relativeToVRT=\"0\">" +
                                io::toUtf8(real) +
                                "</SourceFilename><SourceBand>1</SourceBand></SimpleSource></VRTRasterBand></VRTDataset>";
        writeFile(raster::artefactsDir() / "list.vrt", vrt);
        writeFile(raster::artefactsDir() / "disguised.tif", vrt);
        EXPECT_FALSE(tilers::Raster::openInput(io::toUtf8(raster::artefactsDir() / "list.vrt")));
        EXPECT_FALSE(tilers::Raster::openInput(io::toUtf8(raster::artefactsDir() / "disguised.tif")));

        // A PNG with the name of a GeoTIFF is no GeoTIFF
        const fs::path png = raster::artefactsDir() / "image.tif";
        GDALDatasetH source = GDALOpen(io::toUtf8(real).c_str(), GA_ReadOnly);
        ASSERT_NE(source, nullptr);
        GDALClose(GDALCreateCopy(GDALGetDriverByName("PNG"), io::toUtf8(png).c_str(), source, FALSE, nullptr, nullptr, nullptr));
        GDALClose(source);
        ASSERT_TRUE(fs::exists(png));
        EXPECT_FALSE(tilers::Raster::openInput(io::toUtf8(png)));

        // A GeoTIFF is not a vector, and a raster open does not take a GeoPackage
        EXPECT_FALSE(raster::openArtefactVector(real));
        const fs::path gpkg = raster::artefactsDir() / "layers.gpkg";
        {
            GDALDatasetH dataset = GDALCreate(GDALGetDriverByName("GPKG"), io::toUtf8(gpkg).c_str(), 0, 0, 0, GDT_Unknown, nullptr);
            ASSERT_NE(dataset, nullptr);
            GDALDatasetCreateLayer(dataset, "points", nullptr, wkbPoint, nullptr);
            GDALClose(dataset);
        }
        EXPECT_FALSE(tilers::Raster::openInput(io::toUtf8(gpkg)));
        const auto layers = raster::openArtefactVector(gpkg);
        ASSERT_TRUE(layers);
        EXPECT_EQ(GDALDatasetGetLayerCount(datasetHandle(layers)), 1);

        EXPECT_TRUE(tilers::Raster::openInput(io::toUtf8(real)));
    }

    TEST_F(RasterArtefacts, aRasterWhoseMetadataNamesAnotherDatasetIsReadWithoutIt)
    {
        // The metadata GDAL would open the named dataset from (the overviews of a raster, the arrays of
        // a geolocation) came from a file of a user if it is in a copy: the host reads the raster
        // alone, with its own overviews
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
            const auto raster = tilers::Raster::openInput(io::toUtf8(artefact));
            ASSERT_TRUE(raster);
            EXPECT_TRUE(raster->overviews().empty());
            EXPECT_EQ(raster->file(), artefact);
        }
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
        EXPECT_THROW(raster::requireArtefact(build_ / CogBuildSubfolder / CogFileName), GDALException);

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
        EXPECT_THROW(raster::requireArtefact(planted), GDALException);
    }

    TEST_F(RasterArtefacts, aFileTheHelperJustWroteIsCheckedWhereverItIs)
    {
        // Not an artefact by place, and not refused for it: it is what the helper wrote just now
        const fs::path written = makeGeoTiff(area_->getPath("export.tif"));
        EXPECT_THROW(raster::requireArtefact(written), GDALException);
        EXPECT_TRUE(raster::writtenRasterOpens(written));

        // Only a regular file that is a TIFF
        EXPECT_FALSE(raster::writtenRasterOpens(area_->getPath("missing.tif")));
        EXPECT_FALSE(raster::writtenRasterOpens(area_->getPath("outside")));
        writeFile(area_->getPath("list.vrt"), "<VRTDataset rasterXSize=\"1\" rasterYSize=\"1\"/>");
        EXPECT_FALSE(raster::writtenRasterOpens(area_->getPath("list.vrt")));
        writeFile(area_->getPath("truncated.tif"), std::string("II*\0\x08\0\0\0", 8));
        EXPECT_FALSE(raster::writtenRasterOpens(area_->getPath("truncated.tif")));
#ifndef _WIN32
        fs::create_symlink(written, area_->getPath("link.tif"));
        EXPECT_FALSE(raster::writtenRasterOpens(area_->getPath("link.tif")));
#endif
    }

    // The check of what the helper wrote leaves the file closed: the caller renames it next, which
    // Windows refuses while a process holds it open
    TEST_F(RasterArtefacts, theCheckOfAWrittenRasterLeavesItClosed)
    {
        const fs::path written = makeGeoTiff(area_->getPath("produced.tif"));
        ASSERT_TRUE(raster::writtenRasterOpens(written));
        if (testutil::openDescriptorsOf(written) >= 0)
        {
            EXPECT_EQ(testutil::openDescriptorsOf(written), 0);
        }
        std::error_code ec;
        fs::rename(written, area_->getPath("published.tif"), ec);
        EXPECT_FALSE(ec) << ec.message();
        EXPECT_TRUE(raster::writtenRasterOpens(area_->getPath("published.tif")));
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
        PointCloudInfo pointCloudInfo(const fs::path &) override
        {
            ADD_FAILURE() << "no point cloud info is asked for";
            return PointCloudInfo();
        }
        void thumbnail(const fs::path &, const fs::path &, const raster::ThumbnailParams &) override
        {
            ADD_FAILURE() << "no thumbnail is asked for";
        }
        // An opaque tile of the size asked for, in the format asked for
        void photoTile(const fs::path &path, const fs::path &out, const raster::PhotoTileParams &params) override
        {
            record("photoTile", path);
            lastTileOut = out;
            lastTileParams = params;
            RasterImage image(params.tileSize, params.tileSize, 3, false);
            std::fill(image.color.begin(), image.color.end(), uint8_t{200});
            EncodeOptions options;
            options.format = params.format;
            TileEncoder::write(image, options, io::toUtf8(out));
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
        json buildCopc(const fs::path &, const fs::path &, const json &, const raster::ProgressCallback &) override
        {
            ADD_FAILURE() << "no point cloud is built";
            return json::object();
        }
        void buildVector(const fs::path &, const fs::path &, const json &, const raster::ProgressCallback &) override
        {
            ADD_FAILURE() << "no vector is built";
        }
        void sampleCloud(const fs::path &, const fs::path &, const json &, const raster::ProgressCallback &) override
        {
            ADD_FAILURE() << "no cloud is sampled";
        }
        json operate(const char *op, const fs::path &, const fs::path &, const json &,
                     const raster::ProgressCallback &) override
        {
            ADD_FAILURE() << "no operation: " << op;
            return json::object();
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
        fs::path lastTileOut;
        raster::PhotoTileParams lastTileParams;

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
        RasterTiler tiler(io::toUtf8(artefact), "", 256);
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
        EXPECT_THROW(RasterTiler(io::toUtf8(raw), "", 256), GDALException);
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
        // The clean-up runs when the test asks for it: one that toGeoTIFF ran at random before it touched the
        // aged COG would remove it, and the COG would be converted again
        const ScopedCacheCleanupChanceForTesting noRandomCleanup(0);
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

    // What made the test above fail now and then: a conversion whose clean-up runs (one in a thousand, here
    // every time) removes the COG no request touched for six days before it looks for it, and converts it
    // again, which is harmless
    TEST_F(RasterArtefacts, aConversionThatCleansUpTheCacheFirstConvertsAnAgedCogAgain)
    {
        const ScopedCacheCleanupChanceForTesting everyTime(1);
        const fs::path preparedCog = makeGeoTiff(area_->getPath("prepared") / "cog.tif");
        ConvertingHelper helper(preparedCog);
        const raster::ScopedRasterHelper installed(helper);
        const fs::path raw = area_->getPath("raw.tif");
        writeFile(raw, "bytes that no driver of GDAL opens");

        const fs::path cog = TilerHelper::toGeoTIFF(raw);
        EXPECT_EQ(helper.count("cog"), 1u);
        fs::last_write_time(cog, fs::file_time_type::clock::now() - std::chrono::hours(24 * 6));

        EXPECT_EQ(TilerHelper::toGeoTIFF(raw), cog);
        EXPECT_TRUE(fs::exists(cog));
        EXPECT_EQ(helper.count("cog"), 2u) << "removed by the clean-up, then converted again";
    }

    // The files below the artefacts folder of the cache (none when there is no such folder)
    std::vector<fs::path> filesInArtefacts(const fs::path &cache)
    {
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(cache / "artefacts", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file())
                files.push_back(it->path());
        return files;
    }

    // The photo of the tests of its tiles, with the georeference its EXIF gives it
    struct TestPhoto
    {
        fs::path path;
        raster::PhotoGeoreference georeference;
    };

    TestPhoto downloadPhoto(TestArea &area)
    {
        TestPhoto photo;
        photo.path = area.downloadTestAsset(testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");
        std::string problem;
        const auto georeference = photoGeoreferenceOf(photo.path, problem);
        if (!georeference)
            throw AppException("the test photo has no footprint: " + problem);
        photo.georeference = *georeference;
        return photo;
    }

    // The 256 px tile (TMS row) of zoom z that holds the point (x, y) of EPSG:3857
    TileInfo tileAt(double x, double y, int z)
    {
        const Projected2Di t = GlobalMercator(256).metersToTile(x, y, z);
        return TileInfo(t.x, t.y, z);
    }

    // The 256 px tile (TMS row) in the middle of what the tiler shows at zoom z
    TileInfo middleOf(const Tiler &tiler, int z)
    {
        const BoundingBox<Projected2D> content = *tiler.contentBounds();
        return tileAt((content.min.x + content.max.x) / 2, (content.min.y + content.max.y) / 2, z);
    }

    std::vector<uint8_t> tileOf(Tiler &tiler, const TileInfo &t)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        tiler.tile(t.tz, t.tx, t.ty, ImageFormat::Png, &buffer, &size);
        return testutils::takeBuffer(buffer, size);
    }

    // Which tiles hold some of a footprint: one inside it (no corner of either inside the other), one across an
    // edge, one that only comes within the margin; not one beside it, nor one in the corner of the box of a
    // diamond that the diamond does not reach
    TEST(PhotoTileGeometry, aBoxMeetsTheFootprintWhereverTheyOverlap)
    {
        raster::PhotoGeoreference diamond;
        diamond.width = 100;
        diamond.height = 100;
        diamond.footprint = {{{0.0, 0.001}, {-0.001, 0.0}, {0.0, -0.001}, {0.001, 0.0}}};
        const auto corners = raster::photoCornersInMeters(diamond);
        const double r = corners[3].x; // the half diagonal in metres (111 m)
        const auto box = [](double x0, double y0, double x1, double y1) {
            return BoundingBox<Projected2D>(Projected2D(x0, y0), Projected2D(x1, y1));
        };
        EXPECT_TRUE(raster::footprintMeetsBox(diamond, box(-1, -1, 1, 1), 0)) << "inside";
        EXPECT_TRUE(raster::footprintMeetsBox(diamond, box(-r, -r, r, r), 0)) << "around";
        EXPECT_TRUE(raster::footprintMeetsBox(diamond, box(r / 2 - 1, -1, r, 1), 0)) << "across an edge";
        EXPECT_FALSE(raster::footprintMeetsBox(diamond, box(r + 1, -1, r + 2, 1), 0)) << "beside";
        EXPECT_TRUE(raster::footprintMeetsBox(diamond, box(r + 1, -1, r + 2, 1), 1.5)) << "within the margin";
        EXPECT_FALSE(raster::footprintMeetsBox(diamond, box(0.8 * r, 0.8 * r, r, r), 0)) << "the corner of the box";
        EXPECT_NEAR(raster::photoResolution(diamond), 2 * r / std::hypot(100.0, 100.0), 1e-9);
    }

    // A photo is tiled from itself, one tile per request of the helper: no COG of it is built, and nothing
    // lands in the artefacts of the cache, through the tile requests or a run of the tiler
    TEST_F(RasterArtefacts, aPhotoIsTiledFromItselfAndNothingOfItIsKept)
    {
        const TestPhoto photo = downloadPhoto(*area_);
        const PhotoTiler tiler(photo.path, "", 256, false, photo.georeference);
        const int z = tiler.getMinMaxZ().max;
        const TileInfo middle = middleOf(tiler, z);

        const std::vector<uint8_t> tile = memoryTile(testTile(photo.path, z, middle.tx, middle.ty, 256, false, false));
        const auto image = testutils::decodeImage(tile);
        EXPECT_EQ(image.width, 256);
        EXPECT_GT(std::count(image.alpha().begin(), image.alpha().end(), 255), 1000) << "the photo is in the tile";
        EXPECT_EQ(memoryTile(testTile(photo.path, z, middle.tx, middle.ty, 256, false, false)), tile)
            << "drawn again the same";

        std::ostringstream listed;
        TilerRun run;
        run.zRange = std::to_string(z - 2);
        TilerHelper::runTiler(photo.path, area_->getFolder("tiles"), listed, run);
        EXPECT_FALSE(listed.str().empty());

        const std::vector<fs::path> kept = filesInArtefacts(cache_);
        EXPECT_TRUE(kept.empty()) << io::toUtf8(kept.front());
    }

    // The tile is asked of the helper with the photo, its footprint and the tile; the helper writes it in a
    // folder of the request, which is gone with the request. A tile that holds nothing of the footprint is the
    // blank tile, made without the helper.
    TEST_F(RasterArtefacts, aPhotoTileIsAskedOfTheHelperOnlyWhereThePhotoIs)
    {
        const TestPhoto photo = downloadPhoto(*area_);
        ConvertingHelper helper(makeGeoTiff(area_->getPath("prepared") / "cog.tif"));
        const raster::ScopedRasterHelper installed(helper);
        const PhotoTiler tiler(photo.path, "", 256, false, photo.georeference);
        const int z = tiler.getMinMaxZ().max;
        const TileInfo middle = middleOf(tiler, z);

        uint8_t *buffer = nullptr;
        int size = 0;
        TileRequest request = testTile(photo.path, z, middle.tx, middle.ty, 256, false, false);
        request.format = "webp";
        request.outBuffer = &buffer;
        request.outBufferSize = &size;
        TilerHelper::getTile(request);
        const std::vector<uint8_t> tile = testutils::takeBuffer(buffer, size);

        EXPECT_EQ(helper.count("photoTile"), 1u);
        EXPECT_EQ(helper.count("cog"), 0u) << "no COG of the photo";
        EXPECT_EQ(helper.pathOf("photoTile"), photo.path);
        const raster::PhotoTileParams &sent = helper.lastTileParams;
        EXPECT_EQ(sent.width, photo.georeference.width);
        EXPECT_EQ(sent.height, photo.georeference.height);
        EXPECT_EQ(sent.footprint, photo.georeference.footprint);
        EXPECT_EQ(sent.z, z);
        EXPECT_EQ(sent.x, middle.tx);
        EXPECT_EQ(sent.y, middle.ty);
        EXPECT_TRUE(sent.tms) << "a request whose flag is false names a TMS row";
        EXPECT_EQ(sent.format, ImageFormat::Webp);
        EXPECT_EQ(testutils::decodeImage(tile).width, 256) << "the bytes the helper wrote";
        EXPECT_FALSE(fs::exists(helper.lastTileOut.parent_path())) << "the folder of the request is gone";

        // Far from the photo: blank, and the helper is not asked
        const std::vector<uint8_t> away =
            memoryTile(testTile(photo.path, z, middle.tx + 50, middle.ty, 256, false, false));
        const auto blank = testutils::decodeImage(away);
        EXPECT_EQ(std::count(blank.alpha().begin(), blank.alpha().end(), 0), 256 * 256);
        EXPECT_EQ(helper.count("photoTile"), 1u);
        EXPECT_TRUE(filesInArtefacts(cache_).empty());
    }

    // A copy of the JPEG `from` without its Multi-Picture Format segment (APP2 "MPF"): the copy has no preview,
    // and keeps its EXIF (with the EXIF thumbnail) and the main image
    fs::path withoutPreview(const fs::path &from, const fs::path &to)
    {
        const std::string bytes = readFile(from);
        std::string out = bytes.substr(0, 2);
        size_t at = 2;
        while (at + 4 <= bytes.size() && static_cast<unsigned char>(bytes[at]) == 0xFF)
        {
            const auto marker = static_cast<unsigned char>(bytes[at + 1]);
            const size_t length = (static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]);
            if (marker == 0xDA) // the scan: the rest is the image
                break;
            if (!(marker == 0xE2 && bytes.compare(at + 4, 4, std::string("MPF\0", 4)) == 0))
                out += bytes.substr(at, length + 2);
            at += length + 2;
        }
        out += bytes.substr(at);
        writeFile(to, out);
        return to;
    }

    // The tiles a photo had when they came from the COG of the photo (warped from its footprint by the
    // builder): the same zoom levels and tiles, the photo where it was, and its colours within the
    // difference of the resampling (bilinear from the photo, against the nearest pixel of the averaged
    // levels of a lossy COG). That difference grows where the photo is a few dozen pixels wide: the
    // nearest pixel of foliage at 1/64 of its size is any of its colours. The colours on average are the
    // same at every zoom: a tile drawn from the wrong level (the 4:3 EXIF thumbnail of the 16:9 photo, with
    // its black bands, which GDAL lists among the overviews of a JPEG) or a shifted photo is not. The photo
    // is tiled as it is (its small tiles come from its preview) and without its preview (from its overviews).
    TEST_F(RasterArtefacts, theTilesOfAPhotoAreThoseOfItsCogWithinTheResampling)
    {
        const TestPhoto photo = downloadPhoto(*area_);
        const fs::path cog = build_ / CogBuildSubfolder / CogFileName;
        fs::create_directories(cog.parent_path());
        buildCogInPrivateFolder(io::toUtf8(photo.path), cog, {}, photo.georeference);
        RasterTiler fromCog(io::toUtf8(cog), "", 256, false);
        const int maxZ = fromCog.getMinMaxZ().max;
        const BoundingBox<Projected2D> content = *fromCog.contentBounds();

        for (const fs::path &file : {photo.path, withoutPreview(photo.path, area_->getPath("no-preview.jpg"))})
        {
            SCOPED_TRACE(io::toUtf8(file.filename()));
            PhotoTiler fromPhoto(file, "", 256, false, photo.georeference);
            ASSERT_EQ(fromPhoto.getMinMaxZ().min, fromCog.getMinMaxZ().min);
            ASSERT_EQ(fromPhoto.getMinMaxZ().max, maxZ);
            for (int z = std::max(0, maxZ - 6); z <= maxZ + 1; z++)
            {
                SCOPED_TRACE("zoom " + std::to_string(z));
                ASSERT_EQ(fromPhoto.getTilesForZoomLevel(z).size(), fromCog.getTilesForZoomLevel(z).size());
                ASSERT_EQ(fromPhoto.getMinMaxCoordsForZ(z).min.x, fromCog.getMinMaxCoordsForZ(z).min.x);
                ASSERT_EQ(fromPhoto.getMinMaxCoordsForZ(z).max.y, fromCog.getMinMaxCoordsForZ(z).max.y);

                // The middle of the photo and the tiles of the corners of its box, which hold its edges
                size_t opaque = 0, mismatch = 0, inside = 0;
                double difference = 0;
                std::array<double, 3> bias{};
                const double x0 = content.min.x + 0.01, x1 = content.max.x - 0.01;
                const double y0 = content.min.y + 0.01, y1 = content.max.y - 0.01;
                for (const TileInfo &t : {middleOf(fromCog, z), tileAt(x0, y0, z), tileAt(x0, y1, z),
                                          tileAt(x1, y0, z), tileAt(x1, y1, z)})
                {
                    const auto a = testutils::decodeImage(tileOf(fromCog, t));
                    const auto b = testutils::decodeImage(tileOf(fromPhoto, t));
                    ASSERT_EQ(a.width, b.width);
                    for (size_t i = 0; i < a.alpha().size(); i++)
                    {
                        opaque += a.alpha()[i] > 0;
                        mismatch += (a.alpha()[i] > 0) != (b.alpha()[i] > 0);
                        // The colours of the edge of the footprint mix with the transparency around it
                        if (a.alpha()[i] < 255 || b.alpha()[i] < 255)
                            continue;
                        inside++;
                        for (size_t band = 0; band < 3; band++)
                        {
                            const int d = int(a.bands[band][i]) - int(b.bands[band][i]);
                            difference += std::abs(d);
                            bias[band] += d;
                        }
                    }
                }
                // The edge of the footprint moves by a pixel at most; inside, the colours differ by a few levels
                ASSERT_GT(inside, 0u);
                EXPECT_LE(mismatch, opaque / 50 + 256) << "of " << opaque;
                EXPECT_LT(difference / (3.0 * static_cast<double>(inside)), z >= maxZ - 3 ? 12.0 : 20.0);
                for (const double sum : bias)
                    EXPECT_LT(std::abs(sum / static_cast<double>(inside)), 3.0) << "the colours on average";
            }

            // Far from the footprint both are blank
            const TileInfo middle = middleOf(fromCog, maxZ);
            const TileInfo far(middle.tx + 50, middle.ty, maxZ);
            EXPECT_EQ(tileOf(fromPhoto, far), tileOf(fromCog, far));
        }
    }

    // A file of `kib` KiB in `folder`, last used `age` ago
    fs::path cachedFile(const fs::path &folder, const std::string &name, size_t kib, std::chrono::seconds age)
    {
        fs::create_directories(folder);
        const fs::path file = folder / name;
        writeFile(file, std::string(kib * 1024, 'x'));
        fs::last_write_time(file, fs::file_time_type::clock::now() - age);
        return file;
    }

    // Past its bound, the artefact cache loses its least recently used files (a raster with its
    // statistics, the normalized copies too) until it is within it again; the file just made,
    // the files used in the last minutes and the folders of requests in progress stay
    TEST_F(RasterArtefacts, theCacheLosesItsLeastRecentlyUsedFilesPastItsBound)
    {
        using namespace std::chrono_literals;
        const ScopedProcessConfig bound(json{{"artefactCacheMb", 1}}.dump());
        const fs::path dir = raster::artefactsDir();
        const fs::path a = cachedFile(dir, "a.tif", 400, 3h);
        const fs::path aStats = cachedFile(dir, "a.tif.stats.json", 10, 3h);
        const fs::path b = cachedFile(dir, "b.tif", 400, 2h);
        const fs::path c = cachedFile(dir, "c.tif", 400, 1h);
        const fs::path made = cachedFile(dir, "made.tif", 400, 4h);
        const fs::path inUse = cachedFile(dir, "in-use.tif", 400, 10s);
        const fs::path normalized = cachedFile(raster::normalizedDir(), "n.tif", 400, 5h);
        const fs::path request = cachedFile(dir / "req-cog-running", "cog.tif", 5 * 1024, 6h);

        raster::trimArtefacts(made);

        // 2410 KiB: n, a with its statistics, b and c go (n, a, b, c are the oldest that may go)
        for (const fs::path &gone : {normalized, a, aStats, b, c})
            EXPECT_FALSE(fs::exists(gone)) << gone;
        EXPECT_TRUE(fs::exists(made)) << "the file just made";
        EXPECT_TRUE(fs::exists(inUse)) << "a file used in the last minutes";
        EXPECT_TRUE(fs::exists(request)) << "a request in progress is not the cache's";
    }

    // A file used now and then keeps its modification time between marks, so a reader keeps the
    // version of it it has open (ddb-tilers opens a file whose time changed as a new one, with no
    // block cached); a mark older than the interval is renewed, with the companions that exist
    TEST_F(RasterArtefacts, aUsedArtefactIsMarkedOnceAnIntervalWithItsCompanions)
    {
        using namespace std::chrono_literals;
        const fs::path dir = raster::artefactsDir();
        const fs::path recent = cachedFile(dir, "recent.tif", 1, 10s);
        const auto recentTime = fs::last_write_time(recent);
        raster::markArtefactUsed(recent);
        EXPECT_EQ(fs::last_write_time(recent), recentTime) << "marked a moment ago: left as it is";

        const auto stale = std::chrono::seconds(raster::kArtefactMarkSeconds) + 30s;
        const fs::path old = cachedFile(dir, "old.tif", 1, stale);
        const fs::path oldStats = cachedFile(dir, "old.tif.stats.json", 1, stale);
        const fs::path missing = dir / "old.tif.missing";
        const auto before = fs::file_time_type::clock::now() - 1s;
        raster::markArtefactUsed(old, {oldStats, missing});
        EXPECT_GE(fs::last_write_time(old), before);
        EXPECT_GE(fs::last_write_time(oldStats), before);
        EXPECT_FALSE(fs::exists(missing)) << "a companion that is not there is not made";
    }

    // A file used in the last kArtefactInUseSeconds may carry a mark up to kArtefactMarkSeconds
    // older (it is renewed only after that): the bound leaves it
    TEST_F(RasterArtefacts, theBoundLeavesAFileWhoseMarkIsOlderThanItsLastUse)
    {
        const ScopedProcessConfig bound(json{{"artefactCacheMb", 1}}.dump());
        const fs::path dir = raster::artefactsDir();
        const fs::path used = cachedFile(
            dir, "used.tif", 2048, std::chrono::seconds(raster::kArtefactInUseSeconds + raster::kArtefactMarkSeconds / 2));
        raster::trimArtefacts(fs::path());
        EXPECT_TRUE(fs::exists(used));
    }

    TEST_F(RasterArtefacts, aCacheWithoutABoundKeepsEverything)
    {
        using namespace std::chrono_literals;
        const ScopedProcessConfig unbounded(json{{"artefactCacheMb", 0}}.dump());
        const fs::path old = cachedFile(raster::artefactsDir(), "old.tif", 2048, 4h);
        raster::trimArtefacts(fs::path());
        EXPECT_TRUE(fs::exists(old));
    }

    // The COG made for the tiles of a file is published within the bound: older artefacts go
    TEST_F(RasterArtefacts, aNewArtefactKeepsTheCacheWithinItsBound)
    {
        using namespace std::chrono_literals;
        const ScopedProcessConfig bound(json{{"artefactCacheMb", 1}}.dump());
        const fs::path old = cachedFile(raster::artefactsDir(), "old.tif", 1024, 4h);
        ConvertingHelper helper(makeGeoTiff(area_->getPath("prepared") / "cog.tif"));
        const raster::ScopedRasterHelper installed(helper);
        const fs::path raw = area_->getPath("raw.tif");
        writeFile(raw, "bytes that no driver of GDAL opens");

        const fs::path cog = TilerHelper::toGeoTIFF(raw);

        EXPECT_TRUE(fs::exists(cog));
        EXPECT_FALSE(fs::exists(old)) << "the least recently used artefact made room";
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
        // The clean-up of the cache runs on the way (it looks at the folders, makes none)
        const ScopedCacheCleanupChanceForTesting always(1);
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

        EXPECT_TRUE(raster::writtenRasterOpens(out));
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

        EXPECT_TRUE(raster::writtenRasterOpens(out));
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
