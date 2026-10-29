/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The normalized copies of the rasters of users (artefact_cache.h) and the operation that writes
// them (raster_ops.h normalizeRaster): what a copy keeps of a raster and what it leaves behind,
// the cache it lives in (name, reuse, concurrent callers, cleanup), the size limit, and the
// analyses that read a raw raster through it.

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "artefact_cache.h"
#include "build_layout.h"
#include "cpl_string.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "in_process_raster_helper.h"
#include "json.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_cache.h"
#include "raster_helper.h"
#include "raster_ops.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "tilerhelper.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    constexpr const char *kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    // Counts the requests for a normalized copy; the copies are made by the operation itself
    class CountingHelper : public testutils::InProcessRasterHelper
    {
      public:
        void normalize(const fs::path &path, const fs::path &out, const json &params) override
        {
            requests++;
            InProcessRasterHelper::normalize(path, out, params);
        }

        std::atomic<int> requests{0};
    };

    // The cache of the test, apart from the one of the user
    class ArtefactCache : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            area_ = std::make_unique<TestArea>(std::string("ArtefactCache.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            cache_ = area_->getPath("cache");
            config_ = std::make_unique<ScopedProcessConfig>(json{{"cacheDir", io::toUtf8(cache_)}}.dump());
            scope_ = std::make_unique<raster::ScopedRasterHelper>(helper_);
        }

        void TearDown() override
        {
            RasterCache::instance().clear();
            scope_.reset();
        }

        // A small georeferenced raster
        fs::path makeRaster(const std::string &name, int bands = 3)
        {
            testutils::GeoRasterSpec spec;
            spec.width = 300;
            spec.height = 200;
            spec.bands = bands;
            spec.originX = 1000000.0;
            spec.originY = 5000000.0;
            spec.pixelSize = 2.0;
            spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y * 3) % 256); };
            return testutils::createGeoRaster(area_->getPath(name), spec);
        }

        std::vector<fs::path> normalizedFiles() const
        {
            std::vector<fs::path> files;
            for (const auto &entry : fs::directory_iterator(raster::normalizedDir()))
                files.push_back(entry.path());
            return files;
        }

        std::unique_ptr<TestArea> area_;
        std::unique_ptr<ScopedProcessConfig> config_;
        fs::path cache_;
        CountingHelper helper_;
        std::unique_ptr<raster::ScopedRasterHelper> scope_;
    };

    GDALDatasetUniquePtr openPlain(const fs::path &path)
    {
        return adoptDataset(GDALOpenEx(io::toUtf8(path).c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    }

    std::vector<uint8_t> bandBytes(GDALDataset &dataset, int band)
    {
        GDALRasterBand *b = dataset.GetRasterBand(band);
        const int width = b->GetXSize();
        const int height = b->GetYSize();
        const int size = GDALGetDataTypeSizeBytes(b->GetRasterDataType());
        std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * size);
        EXPECT_EQ(b->RasterIO(GF_Read, 0, 0, width, height, bytes.data(), width, height, b->GetRasterDataType(), 0, 0), CE_None);
        return bytes;
    }

    // ------------------------------------------------------------------ what the copy keeps

    TEST_F(ArtefactCache, theCopyKeepsTheBandsValuesAndPropertiesOfTheRaster)
    {
        // A sensor raster as ODM writes it: 16-bit bands with names, units, scale, offset and
        // no-data values, metadata in the default domain and in two others
        const fs::path source = area_->getPath("sensor.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 64, 48, 4, GDT_UInt16, nullptr));
            ASSERT_TRUE(ds);
            const double gt[6] = {500000, 0.5, 0, 4500000, 0, -0.5};
            ds->SetGeoTransform(const_cast<double *>(gt));
            OGRSpatialReference srs;
            srs.importFromEPSG(32632);
            char *wkt = nullptr;
            srs.exportToWkt(&wkt);
            ds->SetProjection(wkt);
            CPLFree(wkt);
            const GDALColorInterp interp[4] = {GCI_BlueBand, GCI_GreenBand, GCI_RedBand, GCI_NIRBand};
            const char *names[4] = {"Blue", "Green", "Red", "NIR"};
            for (int b = 1; b <= 4; b++)
            {
                GDALRasterBand *band = ds->GetRasterBand(b);
                band->SetColorInterpretation(interp[b - 1]);
                band->SetDescription(names[b - 1]);
                band->SetUnitType("reflectance");
                band->SetScale(0.0001);
                band->SetOffset(0.5 * b);
                band->SetNoDataValue(65535);
                band->SetMetadataItem("WAVELENGTH", std::to_string(450 + b * 50).c_str());
                std::vector<uint16_t> row(64);
                for (int y = 0; y < 48; y++)
                {
                    for (int x = 0; x < 64; x++)
                        row[x] = static_cast<uint16_t>(x * b + y);
                    ASSERT_EQ(band->RasterIO(GF_Write, 0, y, 64, 1, row.data(), 64, 1, GDT_UInt16, 0, 0), CE_None);
                }
            }
            ds->SetMetadataItem("CAMERA", "Sequoia");
            ds->SetMetadataItem("TIRSOURCE", "yes", "SENSOR");
            ds->SetMetadataItem("note", "kept", "xml:SIDECAR");
        }

        const fs::path copy = raster::artefactFor(source);
        EXPECT_NE(copy, source);
        const auto in = openPlain(source);
        const auto out = raster::openArtefactRaster(copy);
        ASSERT_TRUE(in);
        ASSERT_TRUE(out);

        EXPECT_EQ(out->GetRasterXSize(), in->GetRasterXSize());
        EXPECT_EQ(out->GetRasterYSize(), in->GetRasterYSize());
        ASSERT_EQ(out->GetRasterCount(), 4);
        double gtIn[6], gtOut[6];
        in->GetGeoTransform(gtIn);
        out->GetGeoTransform(gtOut);
        for (int i = 0; i < 6; i++)
            EXPECT_DOUBLE_EQ(gtOut[i], gtIn[i]);
        const OGRSpatialReference srsIn(in->GetProjectionRef());
        const OGRSpatialReference srsOut(out->GetProjectionRef());
        EXPECT_TRUE(srsOut.IsSame(&srsIn));

        for (int b = 1; b <= 4; b++)
        {
            GDALRasterBand *i = in->GetRasterBand(b);
            GDALRasterBand *o = out->GetRasterBand(b);
            EXPECT_EQ(o->GetRasterDataType(), i->GetRasterDataType()) << b;
            EXPECT_EQ(o->GetColorInterpretation(), i->GetColorInterpretation()) << b;
            EXPECT_STREQ(o->GetDescription(), i->GetDescription()) << b;
            EXPECT_STREQ(o->GetUnitType(), i->GetUnitType()) << b;
            EXPECT_DOUBLE_EQ(o->GetScale(), i->GetScale()) << b;
            EXPECT_DOUBLE_EQ(o->GetOffset(), i->GetOffset()) << b;
            int hasNoData = 0;
            const double nodata = o->GetNoDataValue(&hasNoData);
            EXPECT_TRUE(hasNoData) << b;
            EXPECT_DOUBLE_EQ(nodata, 65535) << b;
            EXPECT_STREQ(o->GetMetadataItem("WAVELENGTH"), i->GetMetadataItem("WAVELENGTH")) << b;
            // The values are the values: the copy loses nothing
            EXPECT_EQ(bandBytes(*out, b), bandBytes(*in, b)) << b;
        }

        EXPECT_STREQ(out->GetMetadataItem("CAMERA"), "Sequoia");
        EXPECT_STREQ(out->GetMetadataItem("TIRSOURCE", "SENSOR"), "yes");
        ASSERT_NE(out->GetMetadata("xml:SIDECAR"), nullptr);
        EXPECT_STREQ(out->GetMetadata("xml:SIDECAR")[0], "note=kept");
        // The compression is lossless
        EXPECT_STREQ(out->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "DEFLATE");
    }

    TEST_F(ArtefactCache, theCopyKeepsThePaletteAndTheGroundControlPointsOfARasterWithoutGeotransform)
    {
        const fs::path source = area_->getPath("gcps.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 40, 30, 1, GDT_Byte, nullptr));
            GDALColorTable table;
            for (int i = 0; i < 4; i++)
            {
                const GDALColorEntry entry = {static_cast<short>(i * 60), static_cast<short>(255 - i * 60), 7, 255};
                table.SetColorEntry(i, &entry);
            }
            ds->GetRasterBand(1)->SetColorTable(&table);
            ds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
            GDAL_GCP gcps[4] = {{const_cast<char *>("1"), const_cast<char *>(""), 0, 0, 9.0, 45.0, 0},
                                {const_cast<char *>("2"), const_cast<char *>(""), 39, 0, 9.1, 45.0, 0},
                                {const_cast<char *>("3"), const_cast<char *>(""), 39, 29, 9.1, 44.9, 0},
                                {const_cast<char *>("4"), const_cast<char *>(""), 0, 29, 9.0, 44.9, 0}};
            OGRSpatialReference wgs84;
            wgs84.importFromEPSG(4326);
            char *wkt = nullptr;
            wgs84.exportToWkt(&wkt);
            ds->SetGCPs(4, gcps, wkt);
            CPLFree(wkt);
        }

        const auto out = raster::openArtefactRaster(raster::artefactFor(source));
        ASSERT_TRUE(out);
        EXPECT_EQ(out->GetRasterBand(1)->GetColorInterpretation(), GCI_PaletteIndex);
        ASSERT_NE(out->GetRasterBand(1)->GetColorTable(), nullptr);
        EXPECT_EQ(out->GetRasterBand(1)->GetColorTable()->GetColorEntryCount(), 256);
        EXPECT_EQ(out->GetRasterBand(1)->GetColorTable()->GetColorEntry(2)->c1, 120);
        EXPECT_EQ(out->GetGCPCount(), 4);
    }

    TEST_F(ArtefactCache, theCopyLeavesTheDomainsThatNameFilesBehind)
    {
        // A two-page TIFF lists its pages as subdatasets by file name
        const fs::path source = area_->getPath("pages.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            for (int page = 0; page < 2; page++)
            {
                CPLStringList options;
                if (page == 1)
                    options.SetNameValue("APPEND_SUBDATASET", "YES");
                auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 16, 16, 1, GDT_Byte, options.List()));
                ASSERT_TRUE(ds);
                ds->GetRasterBand(1)->Fill(page + 1);
            }
        }
        const auto in = openPlain(source);
        ASSERT_TRUE(in);
        ASSERT_NE(in->GetMetadata("SUBDATASETS"), nullptr) << "the source must name its pages";

        const fs::path copy = raster::artefactFor(source);
        const auto out = raster::openArtefactRaster(copy);
        ASSERT_TRUE(out);
        EXPECT_EQ(out->GetMetadata("SUBDATASETS"), nullptr);
        // Neither the domains GDAL writes from the name of the file nor any item names the source
        for (const char *domain : {"", "IMAGE_STRUCTURE", "DERIVED_SUBDATASETS"})
            for (CSLConstList item = out->GetMetadata(domain); item != nullptr && *item != nullptr; item++)
                EXPECT_EQ(std::string(*item).find(io::toUtf8(source)), std::string::npos) << domain << ": " << *item;
        // The first page is what the copy holds
        EXPECT_EQ(bandBytes(*out, 1), std::vector<uint8_t>(16 * 16, 1));
    }

    TEST(NamingDomains, theDomainsThatPointToOtherDatasetsAreTheOnesThePolicyRefuses)
    {
        for (const char *domain : {"SUBDATASETS", "DERIVED_SUBDATASETS", "OVERVIEWS", "GEOLOCATION", "subdatasets", "Overviews"})
            EXPECT_TRUE(gdalopen::isFileNamingDomain(domain)) << domain;
        for (const char *domain : {"", "IMAGE_STRUCTURE", "RPC", "xml:XMP", "EXIF", "DJI", "COLOR_PROFILE"})
            EXPECT_FALSE(gdalopen::isFileNamingDomain(domain)) << domain;
    }

    // The approximate statistics of GDAL (the statistics of the analyses) read an overview when there is one and
    // else sample whole blocks, so a copy must have the blocks and the overviews of the raster for
    // the same raster to give the same statistics
    TEST_F(ArtefactCache, theCopyGivesTheApproximateStatisticsOfTheRaster)
    {
        const fs::path source = area_->getPath("stripes.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            CPLStringList options;
            options.SetNameValue("BLOCKYSIZE", "8");
            auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 1200, 900, 1, GDT_Float32, options.List()));
            ASSERT_TRUE(ds);
            std::vector<float> row(1200);
            for (int y = 0; y < 900; y++)
            {
                for (int x = 0; x < 1200; x++)
                    row[x] = static_cast<float>((x * 31 + y * y) % 997) + (y % 11 == 0 ? 5000.0f : 0.0f);
                ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, y, 1200, 1, row.data(), 1200, 1, GDT_Float32, 0, 0), CE_None);
            }
            const int levels[2] = {2, 4};
            ASSERT_EQ(ds->BuildOverviews("NEAREST", 2, levels, 0, nullptr, nullptr, nullptr), CE_None);
        }

        const auto in = openPlain(source);
        const auto out = raster::openArtefactRaster(raster::artefactFor(source));
        ASSERT_TRUE(in);
        ASSERT_TRUE(out);

        int inW, inH, outW, outH;
        in->GetRasterBand(1)->GetBlockSize(&inW, &inH);
        out->GetRasterBand(1)->GetBlockSize(&outW, &outH);
        EXPECT_EQ(outW, inW);
        EXPECT_EQ(outH, inH);
        EXPECT_EQ(out->GetRasterBand(1)->GetOverviewCount(), in->GetRasterBand(1)->GetOverviewCount());

        double inStats[4], outStats[4];
        ASSERT_EQ(in->GetRasterBand(1)->ComputeStatistics(TRUE, &inStats[0], &inStats[1], &inStats[2], &inStats[3], nullptr, nullptr), CE_None);
        ASSERT_EQ(out->GetRasterBand(1)->ComputeStatistics(TRUE, &outStats[0], &outStats[1], &outStats[2], &outStats[3], nullptr, nullptr), CE_None);
        for (int i = 0; i < 4; i++)
            EXPECT_DOUBLE_EQ(outStats[i], inStats[i]) << i;
    }

    // ------------------------------------------------------------------ the cache

    TEST_F(ArtefactCache, aRawRasterIsCopiedOnceAndTheCopyIsReused)
    {
        const fs::path source = makeRaster("raw.tif");

        const fs::path first = raster::artefactFor(source);
        EXPECT_EQ(helper_.requests, 1);
        EXPECT_EQ(first.parent_path(), raster::normalizedDir());
        EXPECT_EQ(first.extension(), ".tif");
        EXPECT_EQ(first.stem().string(), raster::artefactKey(source));
        EXPECT_TRUE(raster::isLibddbArtefact(first));
        EXPECT_TRUE(raster::openArtefactRaster(first));

        // Asked again, also through the opening of the analyses: no request
        EXPECT_EQ(raster::artefactFor(source), first);
        EXPECT_TRUE(raster::openInputRaster(io::toUtf8(source)));
        EXPECT_EQ(helper_.requests, 1);
        EXPECT_EQ(normalizedFiles().size(), 1u);
    }

    TEST_F(ArtefactCache, aChangedFileGetsAnotherCopy)
    {
        const fs::path source = makeRaster("raw.tif");
        const fs::path first = raster::artefactFor(source);

        // The same size, a later time
        fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(30));
        const fs::path later = raster::artefactFor(source);
        EXPECT_NE(later, first);
        EXPECT_EQ(helper_.requests, 2);

        // Another size
        makeRaster("raw.tif", 4);
        const fs::path bigger = raster::artefactFor(source);
        EXPECT_NE(bigger, later);
        EXPECT_EQ(helper_.requests, 3);

        // The copies of the earlier files stay until the cleanup
        EXPECT_EQ(normalizedFiles().size(), 3u);
        // Another path with the same content is another file
        fs::copy_file(source, area_->getPath("copy.tif"));
        EXPECT_NE(raster::artefactFor(area_->getPath("copy.tif")), bigger);
    }

    TEST_F(ArtefactCache, anArtefactIsReadAsItIsWithoutARequest)
    {
        const fs::path build = area_->getPath("dataset") / DDB_FOLDER / DDB_BUILD_PATH / kHash;
        fs::create_directories(build / CogBuildSubfolder);
        const fs::path cog = testutils::createGeoRaster(build / CogBuildSubfolder / CogFileName, [] {
            testutils::GeoRasterSpec spec;
            spec.width = 64;
            spec.height = 64;
            spec.bands = 3;
            return spec;
        }());

        EXPECT_EQ(raster::artefactFor(cog), cog);
        EXPECT_TRUE(raster::openInputRaster(io::toUtf8(cog)));
        // A copy is an artefact too: it is not copied again
        const fs::path copy = raster::artefactFor(makeRaster("raw.tif"));
        EXPECT_EQ(raster::artefactFor(copy), copy);
        EXPECT_EQ(helper_.requests, 1);
    }

    TEST_F(ArtefactCache, theCopyIsPublishedInOneRenameAndNothingStaysOfTheRequest)
    {
        const fs::path source = makeRaster("raw.tif");
        const fs::path copy = raster::artefactFor(source);

        // The folders the requests write to are gone, and only the copy is in the cache
        std::vector<std::string> names;
        for (const auto &entry : fs::directory_iterator(raster::artefactsDir()))
            names.push_back(entry.path().filename().string());
        EXPECT_EQ(names, std::vector<std::string>{"normalized"});
        EXPECT_EQ(normalizedFiles(), std::vector<fs::path>{copy});
    }

    TEST_F(ArtefactCache, callersThatAskAtTheSameTimeEachGetAWholeCopy)
    {
        const fs::path source = makeRaster("raw.tif");

        constexpr int kCallers = 6;
        std::vector<fs::path> results(kCallers);
        std::vector<bool> opened(kCallers, false);
        std::vector<std::thread> threads;
        for (int i = 0; i < kCallers; i++)
            threads.emplace_back([&, i] {
                results[i] = raster::artefactFor(source);
                const auto dataset = raster::openArtefactRaster(results[i]);
                opened[i] = dataset && dataset->GetRasterCount() == 3 && !bandBytes(*dataset, 3).empty();
            });
        for (auto &thread : threads)
            thread.join();

        for (int i = 0; i < kCallers; i++)
        {
            EXPECT_EQ(results[i], results[0]);
            EXPECT_TRUE(opened[i]) << i;
        }
        EXPECT_EQ(normalizedFiles().size(), 1u);
        for (const auto &entry : fs::directory_iterator(raster::artefactsDir()))
            EXPECT_EQ(entry.path().filename().string(), "normalized") << "a request left " << entry.path();
    }

    TEST_F(ArtefactCache, theCleanupRemovesTheCopiesThatNobodyAskedForAndKeepsTheFolder)
    {
        const fs::path old = makeRaster("old.tif");
        const fs::path used = makeRaster("used.tif");
        const fs::path fresh = makeRaster("fresh.tif");
        const fs::path oldCopy = raster::artefactFor(old);
        const fs::path usedCopy = raster::artefactFor(used);
        const fs::path freshCopy = raster::artefactFor(fresh);

        const auto longAgo = fs::file_time_type::clock::now() - std::chrono::hours(24 * 10);
        fs::last_write_time(oldCopy, longAgo);
        fs::last_write_time(usedCopy, longAgo);
        // A copy that is asked for is not one nobody asked for
        EXPECT_EQ(raster::artefactFor(used), usedCopy);

        TilerHelper::cleanupUserCache();

        EXPECT_FALSE(fs::exists(oldCopy));
        EXPECT_TRUE(fs::exists(usedCopy));
        EXPECT_TRUE(fs::exists(freshCopy));
        EXPECT_TRUE(fs::is_directory(raster::normalizedDir()));

        // And once everything is old, the folder stays and the next copy is made again
        fs::last_write_time(freshCopy, longAgo);
        fs::last_write_time(usedCopy, longAgo);
        raster::cleanupArtefacts(60 * 60 * 24 * 5);
        EXPECT_TRUE(normalizedFiles().empty());
        EXPECT_EQ(raster::artefactFor(old), oldCopy);
        EXPECT_TRUE(fs::exists(oldCopy));
    }

    // ------------------------------------------------------------------ what cannot be copied

    TEST_F(ArtefactCache, aRasterOfMoreThanABillionPixelsIsRefusedBeforeAnythingIsWritten)
    {
        // 40000 x 30000 pixels: a sparse file of a few kilobytes
        const fs::path source = area_->getPath("huge.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            CPLStringList options;
            options.SetNameValue("TILED", "YES");
            options.SetNameValue("SPARSE_OK", "TRUE");
            auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 40000, 30000, 1, GDT_Byte, options.List()));
            ASSERT_TRUE(ds);
        }

        // Through the program: the limit crosses the process boundary
        raster::ScopedRasterHelper real(raster::GdalUtilHelper::instance());
        try
        {
            raster::artefactFor(source);
            FAIL() << "no exception";
        }
        catch (const raster::RasterNotReadException &e)
        {
            EXPECT_EQ(e.code(), raster::ErrorCode::Limit);
            const std::string message = e.what();
            EXPECT_NE(message.find("build the dataset first"), std::string::npos) << message;
            EXPECT_NE(message.find("40000 x 30000"), std::string::npos) << message;
            static_assert(raster::kMaxNormalizePixels == 1000000000, "the limit is a billion pixels");
        }
        // The analyses open it as they open anything: the limit is an error, not a missing file
        EXPECT_THROW(raster::openInputRaster(io::toUtf8(source)), raster::RasterNotReadException);

        EXPECT_TRUE(normalizedFiles().empty());
        for (const auto &entry : fs::directory_iterator(raster::artefactsDir()))
            EXPECT_EQ(entry.path().filename().string(), "normalized") << "a request left " << entry.path();
    }

    TEST_F(ArtefactCache, aSizeThatDoesNotFitIn32BitsIsCountedInFull)
    {
        // 70000 x 70000 pixels: 4.9 billion, a sparse file of a few kilobytes
        const fs::path source = area_->getPath("wide.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            CPLStringList options;
            options.SetNameValue("TILED", "YES");
            options.SetNameValue("SPARSE_OK", "TRUE");
            auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 70000, 70000, 1, GDT_Byte, options.List()));
            ASSERT_TRUE(ds);
        }
        try
        {
            raster::normalizeRaster(source, area_->getPath("wide-copy.tif"));
            FAIL() << "no exception";
        }
        catch (const raster::RasterOpException &e)
        {
            EXPECT_EQ(e.code(), raster::ErrorCode::Limit);
        }
        EXPECT_FALSE(fs::exists(area_->getPath("wide-copy.tif")));
    }

    TEST_F(ArtefactCache, aFileThatIsNoRasterOpensAsNothing)
    {
        const fs::path text = area_->getPath("notes.tif");
        std::ofstream(text) << "this is not a TIFF";
        EXPECT_FALSE(raster::openInputRaster(io::toUtf8(text)));
        EXPECT_FALSE(raster::openInputRaster(io::toUtf8(area_->getPath("missing.tif"))));
        EXPECT_THROW(raster::artefactFor(text), raster::RasterOpException);
        EXPECT_TRUE(normalizedFiles().empty());
        EXPECT_THROW(raster::artefactFor(area_->getPath("missing.tif")), FSException);
    }

    // A VRT that names a file outside its folder is not copied: the copy would be its pixels
    TEST_F(ArtefactCache, aRasterThatTheOpenPolicyRefusesIsNotCopied)
    {
        const fs::path secret = makeRaster("secret.tif");
        const fs::path vrt = area_->getPath("sub") / "view.vrt";
        fs::create_directories(vrt.parent_path());
        std::ofstream(vrt) << "<VRTDataset rasterXSize=\"300\" rasterYSize=\"200\"><VRTRasterBand dataType=\"Byte\" band=\"1\">"
                              "<SimpleSource><SourceFilename relativeToVRT=\"0\">"
                           << io::toUtf8(secret)
                           << "</SourceFilename><SourceBand>1</SourceBand></SimpleSource></VRTRasterBand></VRTDataset>";
        EXPECT_FALSE(raster::openInputRaster(io::toUtf8(vrt)));
        EXPECT_TRUE(normalizedFiles().empty());
    }

    // ------------------------------------------------------------------ the analyses read through it

    TEST_F(ArtefactCache, anAnalysisOfARawRasterReadsItsCopyAndGivesTheValuesOfTheRaster)
    {
        // A float raster with a no-data value, as a DEM
        const fs::path source = area_->getPath("dem.tif");
        {
            GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
            auto ds = adoptDataset(driver->Create(io::toUtf8(source).c_str(), 40, 30, 1, GDT_Float32, nullptr));
            const double gt[6] = {500000, 10, 0, 4500300, 0, -10};
            ds->SetGeoTransform(const_cast<double *>(gt));
            OGRSpatialReference srs;
            srs.importFromEPSG(32632);
            char *wkt = nullptr;
            srs.exportToWkt(&wkt);
            ds->SetProjection(wkt);
            CPLFree(wkt);
            ds->GetRasterBand(1)->SetNoDataValue(-9999);
            std::vector<float> row(40);
            for (int y = 0; y < 30; y++)
            {
                for (int x = 0; x < 40; x++)
                    row[x] = (x == 5 && y == 5) ? -9999.0f : static_cast<float>(100 + x + y * 2);
                ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, y, 40, 1, row.data(), 40, 1, GDT_Float32, 0, 0), CE_None);
            }
        }

        char *out = nullptr;
        ASSERT_EQ(DDBGetRasterPointValue(io::toUtf8(source).c_str(), 10, 4, &out), DDBERR_NONE) << DDBGetLastError();
        const json point = json::parse(out);
        DDBFree(out);
        EXPECT_DOUBLE_EQ(point.at("value").get<double>(), 100.0 + 10 + 4 * 2.0);
        EXPECT_EQ(helper_.requests, 1);

        // The point value is asked of the same copy
        ASSERT_EQ(DDBGetRasterPointValue(io::toUtf8(source).c_str(), 11, 4, &out), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
        EXPECT_EQ(helper_.requests, 1) << "the copy is made once for all the analyses";

        // The no-data value is kept in the copy: the pixel left out of the statistics is not counted
        ASSERT_EQ(DDBGetRasterAreaStats(io::toUtf8(source).c_str(), 0, 0, 39, 29, &out), DDBERR_NONE) << DDBGetLastError();
        const json stats = json::parse(out);
        DDBFree(out);
        EXPECT_DOUBLE_EQ(stats.at("min").get<double>(), 100.0);
        EXPECT_DOUBLE_EQ(stats.at("max").get<double>(), 100.0 + 39 + 29 * 2.0);
        EXPECT_EQ(stats.at("pixelCount").get<int>(), 40 * 30 - 1);
        EXPECT_EQ(helper_.requests, 1);
    }
}
