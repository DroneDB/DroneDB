/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06: the small geo and raster primitives (angle factors, nodata rule, GeoTIFF options, DEM
// opening, coordinate transformer axis order) on their own.

#include <cmath>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "coordstransformer.h"
#include "dem.h"
#include "dq06_raster_support.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "mathutil.h"
#include "merge_multispectral.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    // DQ-05 left the private `kPi / 180.0` factors of stockpile, volume and raster_profile alone
    // because deg2rad(x) = (x * kPi) / 180.0 rounds differently. DQ-06 replaces them with
    // kDegToRad, which is that same factor, so no output changes; and the expressions that were
    // written as (x * kPi) / 180.0 or (x * 180.0) / kPi became deg2rad / rad2deg.
    TEST(Dq06Math, theTwoWaysToConvertDegreesDifferInTheLastBit)
    {
        int differing = 0;
        for (int i = 0; i <= 3600; i++)
        {
            const double degrees = i * 0.1;
            if (degrees * kDegToRad != deg2rad(degrees))
                differing++;
        }
        EXPECT_GT(differing, 0) << "if this ever fails the two forms agree and kDegToRad is redundant";
        EXPECT_EQ(kDegToRad, kPi / 180.0);
    }

    TEST(Dq06Math, deg2radAndRad2degAreTheExpressionsTheyReplaced)
    {
        for (int i = -3600; i <= 3600; i++)
        {
            const double x = i * 0.1234567;
            EXPECT_EQ(deg2rad(x), x * kPi / 180.0);  // merge_multispectral, tiler
            EXPECT_EQ(rad2deg(x), x * 180.0 / kPi);  // align
        }
    }

    // ---- coordinate transformer -----------------------------------------------------------

    // The process-wide OGR_CT_FORCE_TRADITIONAL_GIS_ORDER setting, for this thread only
    class ThreadForceOrder
    {
        const char *value_;

    public:
        explicit ThreadForceOrder(const char *value) : value_(value)
        {
            CPLSetThreadLocalConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", value);
        }
        ~ThreadForceOrder() { CPLSetThreadLocalConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", nullptr); }
    };

    // The transformer gives lon/lat in, easting/northing out whatever the global setting: it sets
    // the axis order of its two ends itself (DQ-06 removed the global from process_init)
    TEST(Dq06Coords, theTransformerDoesNotNeedTheGlobalAxisOrderSetting)
    {
        for (const char *setting : {"NO", "YES"})
        {
            SCOPED_TRACE(setting);
            const ThreadForceOrder force(setting);

            double x = 10.0, y = 45.0;
            EpsgTransformer(4326, 4258).transform(&x, &y);
            EXPECT_NEAR(x, 10.0, 1e-6);
            EXPECT_NEAR(y, 45.0, 1e-6);

            x = 10.0;
            y = 45.0;
            EpsgTransformer(4326, 3035).transform(&x, &y);
            EXPECT_NEAR(x, 4321000.0 + 0.0, 4e5) << "easting first";
            EXPECT_LT(y, 3000000.0);

            x = 4300000.0;
            y = 2500000.0;
            EpsgTransformer(3035, 4326).transform(&x, &y);
            EXPECT_NEAR(x, 9.7, 0.1);
            EXPECT_NEAR(y, 45.6, 0.1);
        }
    }

    // The raw GDAL object is what the setting changes: without it, the same two SRSs from EPSG
    // codes take latitude first. This is the dependency the transformer removes.
    TEST(Dq06Coords, rawGdalTransformsDependOnTheGlobalSetting)
    {
        OGRSpatialReference wgs84, laea;
        wgs84.importFromEPSG(4326);
        laea.importFromEPSG(3035);

        double x = 45.0, y = 10.0; // latitude first, what the authority order says
        {
            const ThreadForceOrder force("NO");
            std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&wgs84, &laea));
            ASSERT_NE(ct, nullptr);
            ASSERT_TRUE(ct->Transform(1, &x, &y));
            EXPECT_GT(y, 4000000.0) << "authority order in, northing first out";
        }
        x = 10.0;
        y = 45.0;
        {
            const ThreadForceOrder force("YES");
            std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&wgs84, &laea));
            ASSERT_NE(ct, nullptr);
            ASSERT_TRUE(ct->Transform(1, &x, &y));
            EXPECT_GT(x, 4000000.0);
        }
    }

    TEST(Dq06Coords, theSrsPassedInAreCopiedNotChanged)
    {
        OGRSpatialReference wgs84, laea;
        wgs84.importFromEPSG(4326);
        laea.importFromEPSG(3035);
        ASSERT_EQ(wgs84.GetAxisMappingStrategy(), OAMS_AUTHORITY_COMPLIANT);

        const CoordsTransformer t(wgs84, laea);
        EXPECT_EQ(wgs84.GetAxisMappingStrategy(), OAMS_AUTHORITY_COMPLIANT);
        EXPECT_EQ(laea.GetAxisMappingStrategy(), OAMS_AUTHORITY_COMPLIANT);
        EXPECT_NE(t.get(), nullptr);
    }

    TEST(Dq06Coords, tryCreateReportsAMissingTransformationWithoutThrowing)
    {
        OGRSpatialReference local, wgs84;
        local.SetLocalCS("site");
        wgs84.importFromEPSG(4326);
        EXPECT_EQ(CoordsTransformer::tryCreate(local, wgs84), nullptr);
        EXPECT_THROW(CoordsTransformer(local, wgs84), GDALException);
        EXPECT_NE(CoordsTransformer::tryCreate(wgs84, wgs84), nullptr);
    }

    TEST(Dq06Coords, tryTransformReportsAFailedPoint)
    {
        EpsgTransformer toUtm(4326, 32632);
        double x[2] = {10.0, 10.0}, y[2] = {45.0, 45.0};
        EXPECT_TRUE(toUtm.tryTransform(2, x, y));
        EXPECT_NEAR(x[0], 578000.0, 2000.0);

        double bad = 1e30, alsoBad = 1e30;
        EXPECT_FALSE(toUtm.tryTransform(1, &bad, &alsoBad));
        EXPECT_TRUE(toUtm.tryTransform(0, nullptr, nullptr)) << "nothing to transform is a success";
    }

    TEST(Dq06Coords, srsFactoriesGiveTheTraditionalOrder)
    {
        OGRSpatialReference authority;
        authority.importFromEPSG(4326);
        char *wkt = nullptr;
        authority.exportToWkt(&wkt);
        const std::string wgs84Wkt = takeGdalString(wkt);

        SrsPtr made[] = {srsFromEpsg(4258), srsFromUserInput("EPSG:4258"),
                         srsFromProj4("+proj=longlat +datum=WGS84 +no_defs"), srsFromWkt(wgs84Wkt.c_str())};
        for (const auto &srs : made)
        {
            ASSERT_TRUE(srs);
            EXPECT_EQ(srsOf(srs).GetAxisMappingStrategy(), OAMS_TRADITIONAL_GIS_ORDER);
        }
        EXPECT_FALSE(srsFromUserInput("not a crs"));
        EXPECT_FALSE(srsFromUserInput(""));
        EXPECT_FALSE(srsFromProj4(""));
        EXPECT_FALSE(srsFromProj4(nullptr));
    }

    // ---- DEM context ----------------------------------------------------------------------

    fs::path makeDemFile(const fs::path &path, int bands, int epsg, std::optional<double> nodata)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        std::unique_ptr<GDALDataset> ds(drv->Create(path.string().c_str(), 16, 8, bands, GDT_Float32, nullptr));
        const double gt[6] = {500000.0, 2.0, 0.0, 4500000.0, 0.0, -2.0};
        ds->SetGeoTransform(const_cast<double *>(gt));
        if (epsg != 0)
        {
            OGRSpatialReference srs;
            srs.importFromEPSG(epsg);
            char *wkt = nullptr;
            srs.exportToWkt(&wkt);
            ds->SetProjection(wkt);
            CPLFree(wkt);
        }
        if (nodata)
            for (int b = 1; b <= bands; b++)
                ds->GetRasterBand(b)->SetNoDataValue(*nodata);
        return path;
    }

    TEST(Dq06Dem, openDemReadsTheContextOfTheRaster)
    {
        TestArea ta(TEST_NAME, true);
        const DemContext dem = openDem(makeDemFile(ta.getPath("utm.tif"), 2, 32632, -9999.0).string(), 2);
        EXPECT_EQ(dem.width, 16);
        EXPECT_EQ(dem.height, 8);
        EXPECT_EQ(dem.gt[0], 500000.0);
        EXPECT_EQ(dem.gt[5], -2.0);
        EXPECT_EQ(dem.band, GDALGetRasterBand(dem.handle(), 2));
        EXPECT_TRUE(dem.nodata.has);
        EXPECT_EQ(dem.nodata.value, -9999.0);
        EXPECT_FALSE(dem.projection.empty());
        ASSERT_TRUE(dem.srs);
        EXPECT_FALSE(dem.isGeographic);

        const auto toDem = dem.wgs84ToDem();
        ASSERT_NE(toDem, nullptr);
        double x = 9.0, y = 45.0; // the central meridian of zone 32
        toDem->transform(&x, &y);
        EXPECT_NEAR(x, 500000.0, 1.0);
        const auto toWgs = dem.demToWgs84();
        ASSERT_NE(toWgs, nullptr);
        toWgs->transform(&x, &y);
        EXPECT_NEAR(x, 9.0, 1e-9);
        EXPECT_NEAR(y, 45.0, 1e-9);
    }

    TEST(Dq06Dem, aGeographicDemAndAnUnreferencedOne)
    {
        TestArea ta(TEST_NAME, true);
        const DemContext geographic = openDem(makeDemFile(ta.getPath("etrs89.tif"), 1, 4258, std::nullopt).string());
        EXPECT_TRUE(geographic.isGeographic);
        EXPECT_FALSE(geographic.nodata.has);
        EXPECT_EQ(geographic.band, GDALGetRasterBand(geographic.handle(), 1));

        // No CRS: no transformers, map coordinates are WGS84
        const DemContext bare = openDem(makeDemFile(ta.getPath("bare.tif"), 1, 0, std::nullopt).string());
        EXPECT_FALSE(bare.srs);
        EXPECT_FALSE(bare.isGeographic);
        EXPECT_EQ(bare.projection, "");
        EXPECT_EQ(bare.wgs84ToDem(), nullptr);
        EXPECT_EQ(bare.demToWgs84(), nullptr);
    }

    TEST(Dq06Dem, openDemErrors)
    {
        TestArea ta(TEST_NAME, true);
        const std::string missing = ta.getPath("missing.tif").string();
        try
        {
            openDem(missing);
            FAIL();
        }
        catch (const GDALException &e)
        {
            EXPECT_EQ(std::string(e.what()), "Cannot open raster: " + missing);
        }

        const fs::path plain = ta.getPath("plain.tif");
        {
            GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
            std::unique_ptr<GDALDataset> ds(drv->Create(plain.string().c_str(), 4, 4, 1, GDT_Byte, nullptr));
        }
        try
        {
            openDem(plain.string());
            FAIL();
        }
        catch (const GDALException &e)
        {
            EXPECT_EQ(std::string(e.what()), "Raster has no geotransform: " + plain.string());
        }

        const std::string ok = makeDemFile(ta.getPath("ok.tif"), 1, 32632, std::nullopt).string();
        EXPECT_THROW(openDem(ok, 0), InvalidArgsException);
        EXPECT_THROW(openDem(ok, 2), InvalidArgsException);
    }

    // ---- GeoTIFF creation options ----------------------------------------------------------

    std::vector<std::string> asVector(const CPLStringList &list)
    {
        std::vector<std::string> out;
        for (int i = 0; i < list.Count(); i++)
            out.emplace_back(list[i]);
        return out;
    }

    TEST(Dq06Gtiff, theOptionsAreKeyValueStringsInTheOrderTheyWereAdded)
    {
        GtiffOptions co;
        co.compress(GtiffOptions::Codec::Deflate).predictorFor(GDT_Byte).tiled(256).bigTiffIfSafer().set("ZLEVEL", "2").add("NUM_THREADS=3");
        EXPECT_EQ(asVector(co.list()),
                  (std::vector<std::string>{"COMPRESS=DEFLATE", "PREDICTOR=2", "TILED=YES", "BLOCKXSIZE=256",
                                            "BLOCKYSIZE=256", "BIGTIFF=IF_SAFER", "ZLEVEL=2", "NUM_THREADS=3"}));
    }

    TEST(Dq06Gtiff, theCodecsAndThePredictorOfTheSampleType)
    {
        EXPECT_EQ(asVector(GtiffOptions().compress(GtiffOptions::Codec::Lzw).list()), std::vector<std::string>{"COMPRESS=LZW"});
        EXPECT_EQ(asVector(GtiffOptions().compress(GtiffOptions::Codec::Jpeg).list()), std::vector<std::string>{"COMPRESS=JPEG"});
        for (const GDALDataType type : {GDT_Byte, GDT_Int16, GDT_UInt16, GDT_Int32, GDT_UInt32})
            EXPECT_EQ(asVector(GtiffOptions().predictorFor(type).list()), std::vector<std::string>{"PREDICTOR=2"}) << GDALGetDataTypeName(type);
        for (const GDALDataType type : {GDT_Float32, GDT_Float64})
            EXPECT_EQ(asVector(GtiffOptions().predictorFor(type).list()), std::vector<std::string>{"PREDICTOR=3"}) << GDALGetDataTypeName(type);
        EXPECT_EQ(asVector(GtiffOptions().predictorAuto().list()), std::vector<std::string>{"PREDICTOR=YES"});
        EXPECT_EQ(asVector(GtiffOptions().tiled().list()), std::vector<std::string>{"TILED=YES"}) << "no block size without one";
    }

    TEST(Dq06Gtiff, appendToPutsCoInFrontOfEveryOption)
    {
        CPLStringList args;
        args.AddString("-of");
        args.AddString("GTiff");
        GtiffOptions().compress(GtiffOptions::Codec::Lzw).bigTiffIfSafer().appendTo(args);
        EXPECT_EQ(asVector(args),
                  (std::vector<std::string>{"-of", "GTiff", "-co", "COMPRESS=LZW", "-co", "BIGTIFF=IF_SAFER"}));
    }

    // The size of a GeoTIFF of the three bands, JPEG at `quality` (what a merge of three Byte
    // rasters writes when the quality it asks for is applied)
    uintmax_t jpegReferenceSize(const std::vector<fs::path> &inputs, const fs::path &output, int quality)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        CPLStringList co;
        co.AddString("COMPRESS=JPEG");
        co.AddString(("JPEG_QUALITY=" + std::to_string(quality)).c_str());
        co.AddString("TILED=YES");
        {
            std::unique_ptr<GDALDataset> out(drv->Create(output.string().c_str(), 64, 64, 3, GDT_Byte, co.List()));
            for (int b = 0; b < 3; b++)
            {
                std::unique_ptr<GDALDataset> in(static_cast<GDALDataset *>(GDALOpen(inputs[b].string().c_str(), GA_ReadOnly)));
                std::vector<uint8_t> plane(64 * 64);
                dq06::checkIo(in->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, plane.data(), 64, 64, GDT_Byte, 0, 0));
                dq06::checkIo(out->GetRasterBand(b + 1)->RasterIO(GF_Write, 0, 0, 64, 64, plane.data(), 64, 64, GDT_Byte, 0, 0));
            }
        }
        return fs::file_size(output);
    }

    // The merge asks for JPEG quality 90, with the key of the GTiff driver (JPEG_QUALITY). It wrote
    // QUALITY, which GTiff ignores ("driver GTiff does not support creation option QUALITY"), so
    // every JPEG merge came out at GDAL's default of 75; and it passed PREDICTOR with JPEG, which
    // GDAL ignores too.
    TEST(Dq06Gtiff, aJpegMergeIsWrittenAtQuality90)
    {
        TestArea ta(TEST_NAME, true);
        std::vector<fs::path> inputs;
        std::vector<std::string> paths;
        for (int i = 0; i < 3; i++)
        {
            inputs.push_back(dq06::makeRaster(ta.getPath("band" + std::to_string(i) + ".tif"), 1, GDT_Byte, false, std::nullopt, 64));
            paths.push_back(inputs.back().string());
        }
        const fs::path merged = ta.getPath("merged.tif");
        mergeMultispectral(paths, merged.string());

        const uintmax_t q75 = jpegReferenceSize(inputs, ta.getPath("q75.tif"), 75);
        const uintmax_t q90 = jpegReferenceSize(inputs, ta.getPath("q90.tif"), 90);
        ASSERT_GT(q90, q75 + q75 / 10) << "the references must differ for the test to mean anything";

        const uintmax_t size = fs::file_size(merged);
        EXPECT_GT(size, q75 + (q90 - q75) / 2) << "closer to quality 90 (" << q90 << ") than to 75 (" << q75 << ")";
    }
}
