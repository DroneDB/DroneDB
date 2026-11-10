/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The thermal grid of a raster GDAL reads, read a window at a time (ThermalBandReader): the point of the fallback of
// getRasterPointJson reads its pixel, the area of the fallback of getRasterAreaStatsJson and the value info of a thermal
// raster whose statistics GDAL cannot give stream their pixels, so that what a request holds does not grow with the
// raster (thermal_internal.h kMaxThermalGridBufferBytes), and their JSON is that of the grid read whole, as it was read
// before (thermalGridPointJson and thermalGridAreaStatsJson of the temperatures and raw counts of the band).

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "exceptions.h"
#include "gdal_inc.h"
#include "json.h"
#include "raster_analysis.h"
#include "thermal.h"
#include "thermal_internal.h"

namespace
{
    using namespace ddb;

    const char *const kFlirXmp =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
        "<rdf:Description rdf:about='' xmlns:FLIR='http://ns.flir.com/xmp/1.0/' FLIR:PlanckR1='21106.77'"
        " FLIR:PlanckB='1501' FLIR:PlanckF='1' FLIR:PlanckO='-7340' FLIR:PlanckR2='0.012545258'/>"
        "</rdf:RDF></x:xmpmeta><?xpacket end='w'?>";

    // A tiled (256 x 256) one-band GeoTIFF of width x height pixels whose pixel (x, y) holds value(x, y), with the unit
    // type and the XMP packet given
    struct BandSpec
    {
        int width = 1500;
        int height = 1000;
        GDALDataType type = GDT_Float32;
        std::function<double(int, int)> value;
        std::string unit;
        std::string xmp;
    };

    fs::path makeBand(const fs::path &path, const BandSpec &spec)
    {
        char **opts = CSLSetNameValue(nullptr, "TILED", "YES");
        GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), spec.width, spec.height, 1,
                                     spec.type, opts);
        CSLDestroy(opts);
        if (ds == nullptr)
            throw std::runtime_error("Cannot create the test raster");
        const double gt[6] = {1670000.0, 4.0, 0.0, 5645600.0, 0.0, -4.0};
        GDALSetGeoTransform(ds, const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(3857);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        GDALSetProjection(ds, wkt);
        CPLFree(wkt);

        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        std::vector<double> row(static_cast<size_t>(spec.width));
        for (int y = 0; y < spec.height; y++)
        {
            for (int x = 0; x < spec.width; x++)
                row[static_cast<size_t>(x)] = spec.value(x, y);
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, y, spec.width, 1, row.data(), spec.width, 1, GDT_Float64, 0, 0),
                      CE_None);
        }
        if (!spec.unit.empty())
            GDALSetRasterUnitType(band, spec.unit.c_str());
        if (!spec.xmp.empty())
        {
            char *xmp[] = {const_cast<char *>(spec.xmp.c_str()), nullptr};
            GDALSetMetadata(ds, xmp, "xml:XMP");
        }
        GDALClose(ds);
        return path;
    }

    // The temperatures and raw counts of the band read whole, as the fallbacks read them before
    TemperatureData wholeGrid(const ThermalBandReader &band, RawThermalData &raw)
    {
        TemperatureData td;
        td.width = band.width();
        td.height = band.height();
        td.hasGeoTransform = band.hasGeoTransform();
        std::copy(band.geoTransform(), band.geoTransform() + 6, td.geoTransform);
        td.projection = band.projection();
        std::vector<uint16_t> counts;
        EXPECT_TRUE(band.read(0, 0, td.width, td.height, td.temperatures, counts));
        raw = RawThermalData();
        if (band.hasRawCounts())
        {
            raw.data = std::move(counts);
            raw.width = td.width;
            raw.height = td.height;
            raw.valid = true;
        }
        return td;
    }

    // What a call answers: its JSON, else the type and the message of its exception
    std::string answer(const std::function<std::string()> &call)
    {
        try
        {
            return call();
        }
        catch (const NoRasterDataException &e)
        {
            return std::string("NoRasterDataException: ") + e.what();
        }
        catch (const RefusedArgumentException &e)
        {
            return std::string("RefusedArgumentException: ") + e.what();
        }
        catch (const AppException &e)
        {
            return std::string("AppException: ") + e.what();
        }
    }

    std::vector<RasterValueOptions> optionsToCheck(double nodata)
    {
        std::vector<RasterValueOptions> all(4);
        all[1].nodata = nodata;
        all[2].formula = "KELVIN";
        all[3].nodata = nodata;
        all[3].formula = "FAHRENHEIT";
        return all;
    }

    // Every point and area of the band read in windows answers what the grid of the band read whole answers
    void expectTheAnswersOfTheGridReadWhole(const fs::path &path, double nodata)
    {
        const auto band = ThermalBandReader::open(path.string());
        ASSERT_NE(band, nullptr);
        RawThermalData raw;
        const TemperatureData td = wholeGrid(*band, raw);
        const int w = band->width();
        const int h = band->height();

        const int points[][2] = {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {w / 2, h / 2}, {w - 1, h - 1}, {w, 0}, {-1, 3}};
        const int areas[][4] = {{0, 0, w - 1, h - 1}, {w - 1, h - 1, 0, 0}, {7, 600, 1400, 900}, {3, 3, 3, 3},
                                {0, 0, 2, 1},         {-10, -10, w + 10, 4}, {w + 1, 0, w + 5, 5}};
        for (const RasterValueOptions &options : optionsToCheck(nodata))
        {
            SCOPED_TRACE("nodata " + (options.nodata ? std::to_string(*options.nodata) : std::string("none")) +
                         ", formula " + options.formula);
            for (const auto &p : points)
            {
                const std::string whole = answer([&] { return thermalGridPointJson(td, raw, p[0], p[1], options); });
                EXPECT_EQ(answer([&] { return thermalBandPointJson(*band, p[0], p[1], options); }), whole)
                    << p[0] << ", " << p[1];
            }
            for (const auto &a : areas)
            {
                const std::string whole =
                    answer([&] { return thermalGridAreaStatsJson(td, raw, a[0], a[1], a[2], a[3], options); });
                EXPECT_EQ(answer([&] { return thermalBandAreaStatsJson(*band, a[0], a[1], a[2], a[3], options); }),
                          whole)
                    << a[0] << ", " << a[1] << " - " << a[2] << ", " << a[3];
            }
        }
    }

    // A Float32 band of 1500 x 1000 pixels (two windows of rows) with temperatures in K, NaN, values out of the range
    // of a sensor and both zeros
    double kelvinValue(int x, int y)
    {
        if (x == 0 && y == 0)
            return std::numeric_limits<double>::quiet_NaN();
        if (x == 1 && y == 0)
            return -9999.0;
        if (x == 2 && y == 0)
            return 2000.0;
        if (x == 0 && y == 1)
            return -0.0;
        if (x == 1 && y == 1)
            return 0.0;
        return 250.0 + std::fmod(x * 0.37 + y * 1.13 + (x * y % 97) * 0.011, 120.0);
    }

    TEST(ThermalBandWindows, theDirectTemperaturesOfABandAnswerAsTheirGridReadWhole)
    {
        TestArea ta(TEST_NAME, true);
        BandSpec spec;
        spec.value = kelvinValue;
        spec.unit = "K";
        expectTheAnswersOfTheGridReadWhole(makeBand(ta.getPath("kelvin.tif"), spec), 0.0);

        spec.type = GDT_Float64;
        expectTheAnswersOfTheGridReadWhole(makeBand(ta.getPath("kelvin64.tif"), spec), 0.0);
    }

    TEST(ThermalBandWindows, theRawCountsOfABandAnswerAsTheirGridReadWhole)
    {
        TestArea ta(TEST_NAME, true);
        BandSpec spec;
        spec.type = GDT_UInt16;
        // 1000 (no temperature in the calibration) left of the column 30, 0 at (5, 5), counts of 13 to 64 C elsewhere
        spec.value = [](int x, int y) {
            if (x < 30)
                return 1000.0;
            if (x == 5 && y == 5)
                return 0.0;
            return 15000.0 + ((x * 31 + y * 17) % 4000);
        };
        spec.xmp = kFlirXmp;
        const fs::path flir = makeBand(ta.getPath("flir.tif"), spec);
        {
            const auto band = ThermalBandReader::open(flir.string());
            ASSERT_NE(band, nullptr);
            ASSERT_TRUE(band->hasRawCounts());
            std::vector<float> t;
            std::vector<uint16_t> c;
            ASSERT_TRUE(band->read(40, 0, 1, 1, t, c));
            ASSERT_GT(t[0], 0.0f) << "the band must be calibrated by its XMP";
        }
        expectTheAnswersOfTheGridReadWhole(flir, 1000.0);

        // Without the Planck constants, the counts are the values
        spec.xmp.clear();
        spec.unit = "K";
        expectTheAnswersOfTheGridReadWhole(makeBand(ta.getPath("raw.tif"), spec), 1000.0);
    }

    // The statistics of an area with more values than are kept in memory: the count, the range, the bounds and the
    // median are those of the values sorted (a median between two values of the same and of two bins of the upper bits
    // of their keys, of an odd and an even count, negative values and both zeros), the mean and the standard deviation
    // those of the compensated sums
    TEST(ThermalBandWindows, anAreaWithMoreValuesThanAreKeptHasTheStatisticsOfTheValuesSorted)
    {
        const auto statsOf = [](const std::vector<float> &values, std::size_t limit) {
            TemperatureData td;
            td.width = static_cast<int>(values.size());
            td.height = 1;
            td.temperatures = values;
            const ScopedAreaValuesInMemoryLimitForTesting kept(limit);
            return json::parse(thermalGridAreaStatsJson(td, RawThermalData(), 0, 0, td.width - 1, 0));
        };
        const std::vector<std::vector<float>> cases = {
            {1.0f, 2.0f},                                  // the middle ranks in two bins
            {1.0f, 1.0000001f},                            // in one bin
            {5.0f, -3.5f, 12.25f, 0.0f, -0.0f, 7.0f, 7.0f}, // odd, negative values and zeros
            {-20.0f, -20.0f, -19.5f, -100.0f, 999.0f, -273.0f, 0.5f, 0.25f},
            {42.0f},
        };
        for (const auto &values : cases)
        {
            const json kept = statsOf(values, kMaxAreaValuesInMemory);
            for (const std::size_t limit : {std::size_t(0), std::size_t(1), values.size() - 1})
            {
                const json read = statsOf(values, limit);
                SCOPED_TRACE(kept.dump() + " / " + read.dump());
                for (const char *key : {"min", "max", "median", "pixelCount", "bounds", "unit", "isThermal"})
                    EXPECT_EQ(read[key], kept[key]) << key;
                EXPECT_NEAR(read["mean"].get<double>(), kept["mean"].get<double>(),
                            2e-6 * std::fabs(kept["mean"].get<double>()));
                EXPECT_NEAR(read["stddev"].get<double>(), kept["stddev"].get<double>(),
                            2e-6 * std::fabs(kept["stddev"].get<double>()));
            }
        }

        // No value: a NoRasterDataException, with or without the values in memory
        TemperatureData none;
        none.width = 3;
        none.height = 1;
        none.temperatures = {std::nanf(""), 5000.0f, -300.0f};
        for (const std::size_t limit : {std::size_t(0), kMaxAreaValuesInMemory})
        {
            const ScopedAreaValuesInMemoryLimitForTesting kept(limit);
            EXPECT_THROW(thermalGridAreaStatsJson(none, RawThermalData(), 0, 0, 2, 0), NoRasterDataException);
        }
    }

    // The same on the 1.5 million pixels of a band read in windows, kept in memory and read twice
    TEST(ThermalBandWindows, theAreaOfABandReadTwiceHasTheStatisticsOfItsValuesSorted)
    {
        TestArea ta(TEST_NAME, true);
        BandSpec spec;
        spec.value = kelvinValue;
        spec.unit = "K";
        const fs::path path = makeBand(ta.getPath("kelvin.tif"), spec);
        const auto band = ThermalBandReader::open(path.string());
        ASSERT_NE(band, nullptr);

        RasterValueOptions kelvin;
        kelvin.formula = "KELVIN";
        for (const RasterValueOptions &options : {RasterValueOptions(), kelvin})
        {
            const json kept = json::parse(thermalBandAreaStatsJson(*band, 0, 0, 1499, 999, options));
            ASSERT_EQ(kept["pixelCount"], 1500 * 1000 - 3);
            const ScopedAreaValuesInMemoryLimitForTesting limit(1000);
            const json read = json::parse(thermalBandAreaStatsJson(*band, 0, 0, 1499, 999, options));
            SCOPED_TRACE(kept.dump() + " / " + read.dump());
            for (const char *key : {"min", "max", "median", "pixelCount", "bounds", "unit", "isThermal"})
                EXPECT_EQ(read[key], kept[key]) << key;
            EXPECT_NEAR(read["mean"].get<double>(), kept["mean"].get<double>(), 2e-6 * kept["mean"].get<double>());
            EXPECT_NEAR(read["stddev"].get<double>(), kept["stddev"].get<double>(),
                        2e-6 * kept["stddev"].get<double>());
        }
    }

    // A sparse tiled GTiff of width x height UInt16 pixels in blocks of 512 x 512, none written: every pixel is the
    // NoData value 7 of the band, in K (a thermal raster by its unit), and the file takes a few KB
    void makeSparseKelvin(const fs::path &path, int width, int height)
    {
        char **opts = CSLSetNameValue(nullptr, "TILED", "YES");
        opts = CSLSetNameValue(opts, "BLOCKXSIZE", "512");
        opts = CSLSetNameValue(opts, "BLOCKYSIZE", "512");
        opts = CSLSetNameValue(opts, "SPARSE_OK", "TRUE");
        GDALDatasetH ds =
            GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), width, height, 1, GDT_UInt16, opts);
        CSLDestroy(opts);
        ASSERT_NE(ds, nullptr);
        const GDALRasterBandH b = GDALGetRasterBand(ds, 1);
        ASSERT_EQ(GDALSetRasterNoDataValue(b, 7.0), CE_None);
        ASSERT_EQ(GDALSetRasterUnitType(b, "K"), CE_None);
        GDALClose(ds);
    }

    // A thermal raster of 16384 x 16384 UInt16 pixels (makeSparseKelvin), whose grid read whole took 10 bytes a pixel
    // (2.7 GB): its value info (the statistics of the band model have no pixel), its area and its point hold at most
    // kMaxThermalGridBufferBytes of buffers and read at most kThermalGridWindowPixels at a time, and so does the area of
    // a raster whose rows are wider than a window
    TEST(ThermalBandWindows, theValueInfoAreaAndPointOfALargeThermalRasterHoldBoundedBuffers)
    {
        TestArea ta(TEST_NAME, true);
        constexpr int kSide = 16384;
        const fs::path path = ta.getPath("sparse.tif");
        makeSparseKelvin(path, kSide, kSide);
        ASSERT_FALSE(HasFatalFailure());
        constexpr uint64_t kPixels = static_cast<uint64_t>(kSide) * kSide;
        ASSERT_GT(kPixels, kMaxAreaValuesInMemory);
        auto &buffers = thermalGridBuffersForTesting();
        const auto expectBounded = [&](uint64_t largestWindow) {
            EXPECT_GT(buffers.peakBytes, 0u);
            EXPECT_LE(buffers.peakBytes, kMaxThermalGridBufferBytes);
            EXPECT_LE(kMaxThermalGridBufferBytes, 64ull << 20);
            EXPECT_EQ(buffers.largestWindowPixels, largestWindow);
            EXPECT_EQ(buffers.heldBytes, 0u) << "every buffer is released";
        };

        // The value info: every pixel, read 64 rows (2^20 pixels) at a time
        buffers.reset();
        const json info = json::parse(getRasterValueInfoJson(path.string()));
        EXPECT_EQ(info["dataType"], "Unknown") << "the thermal fallback: " << info.dump();
        EXPECT_EQ(info["width"], kSide);
        EXPECT_EQ(info["height"], kSide);
        EXPECT_EQ(info["validPixelCount"], kPixels);
        EXPECT_EQ(info["valueMin"].get<double>(), 7.0);
        EXPECT_EQ(info["valueMax"].get<double>(), 7.0);
        expectBounded(static_cast<uint64_t>(kSide) * 64);

        const auto band = ThermalBandReader::open(path.string());
        ASSERT_NE(band, nullptr);

        // The area: read twice, the median selected from the histograms of the keys
        buffers.reset();
        const json area = json::parse(thermalBandAreaStatsJson(*band, 0, 0, kSide - 1, kSide - 1));
        EXPECT_EQ(area["pixelCount"], kPixels);
        EXPECT_EQ(area["min"].get<double>(), 7.0);
        EXPECT_EQ(area["max"].get<double>(), 7.0);
        EXPECT_EQ(area["median"].get<double>(), 7.0);
        EXPECT_EQ(area["mean"].get<double>(), 7.0);
        EXPECT_EQ(area["stddev"].get<double>(), 0.0);
        expectBounded(static_cast<uint64_t>(kSide) * 64);

        // The point: its pixel alone
        buffers.reset();
        const json point = json::parse(thermalBandPointJson(*band, kSide - 1, kSide - 1));
        EXPECT_EQ(point["rawValue"], 7);
        EXPECT_EQ(buffers.largestWindowPixels, 1u);

        // Rows wider than a window are read in parts of a window
        const fs::path wide = ta.getPath("wide.tif");
        const int wideWidth = static_cast<int>(kThermalGridWindowPixels) + 300;
        makeSparseKelvin(wide, wideWidth, 3);
        ASSERT_FALSE(HasFatalFailure());
        const auto wideBand = ThermalBandReader::open(wide.string());
        ASSERT_NE(wideBand, nullptr);
        buffers.reset();
        const json rows = json::parse(thermalBandAreaStatsJson(*wideBand, 0, 0, wideWidth - 1, 2));
        EXPECT_EQ(rows["pixelCount"], 3 * static_cast<int64_t>(wideWidth));
        EXPECT_EQ(rows["median"].get<double>(), 7.0);
        expectBounded(static_cast<uint64_t>(kThermalGridWindowPixels));
    }
}
