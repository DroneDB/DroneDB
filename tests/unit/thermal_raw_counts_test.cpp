/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The UInt16 band of a thermal raster (U-603: FR-RAS-14, FR-XC-24, D-022): raw sensor counts, temperatures with the
// calibration of the file, or physical values with a scale and an offset. The value endpoints (value info, point, area,
// profile) follow one rule: raw counts without a calibration have no unit and refuse a thermal formula; a band with a
// scale or an offset (the Landsat encoding) holds physical values (raw * scale + offset), never raw counts. The expected
// numbers are literals computed outside the code under test: 45000 * 0.00341802 + 149 = 302.8109, 40000 -> 285.7208,
// 49900 -> 319.5592, 41000 -> 289.1388, 42000 -> 292.5568; the Planck formula (R1 21106.77, B 1501, F 1, O -7340,
// R2 0.012545258) of the raw count 16204 is 12.6860 C, and the raw count 1000 has no temperature in it (the argument of
// the logarithm is negative).

#include <functional>
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
#include "raster_profile.h"

namespace
{
    using namespace ddb;

    const char *const kCelsius = "\xC2\xB0" "C";

    constexpr int kEpsg = 3857;
    constexpr double kOriginX = 1670000.0;
    constexpr double kOriginY = 5645600.0;
    constexpr double kPixel = 4.0;
    constexpr int kSize = 100;

    // A one-band UInt16 GeoTIFF of kSize x kSize pixels whose pixel (x, y) holds value(x, y)
    struct UInt16Spec
    {
        std::function<int(int, int)> value;
        std::string description;        // the description of the band ("thermal": a thermal raster by its metadata)
        bool landsat = false;           // the scale and the offset of Landsat Collection 2 (0.00341802, 149)
        std::string xmp;                // an XMP packet
    };

    fs::path makeUInt16Raster(const fs::path &path, const UInt16Spec &spec)
    {
        GDALDatasetH hDs = GDALCreate(GDALGetDriverByName("GTiff"), path.string().c_str(), kSize, kSize, 1, GDT_UInt16,
                                      nullptr);
        if (!hDs)
            throw std::runtime_error("Cannot create the test raster");
        const double gt[6] = {kOriginX, kPixel, 0.0, kOriginY, 0.0, -kPixel};
        GDALSetGeoTransform(hDs, const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(kEpsg);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);

        GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
        std::vector<uint16_t> row(static_cast<size_t>(kSize));
        for (int y = 0; y < kSize; y++)
        {
            for (int x = 0; x < kSize; x++)
                row[static_cast<size_t>(x)] = static_cast<uint16_t>(spec.value(x, y));
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, y, kSize, 1, row.data(), kSize, 1, GDT_UInt16, 0, 0), CE_None);
        }
        if (!spec.description.empty())
            GDALSetDescription(band, spec.description.c_str());
        if (spec.landsat)
        {
            GDALSetRasterScale(band, 0.00341802);
            GDALSetRasterOffset(band, 149.0);
        }
        if (!spec.xmp.empty())
        {
            char *xmp[] = {const_cast<char *>(spec.xmp.c_str()), nullptr};
            GDALSetMetadata(hDs, xmp, "xml:XMP");
        }
        GDALFlushCache(hDs);
        GDALClose(hDs);
        return path;
    }

    // The profile along the row 50 from the centre of the column `from` to the centre of the column `to`, in WGS84
    std::string rowLine(int from, int to)
    {
        const auto wgs84 = [](int col) {
            double x = kOriginX + (col + 0.5) * kPixel;
            double y = kOriginY - 50.5 * kPixel;
            OGRSpatialReference source, target;
            source.importFromEPSG(kEpsg);
            target.importFromEPSG(4326);
            source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&source, &target));
            ct->Transform(1, &x, &y);
            return std::make_pair(x, y);
        };
        const auto a = wgs84(from);
        const auto b = wgs84(to);
        return json{{"type", "LineString"}, {"coordinates", {{a.first, a.second}, {b.first, b.second}}}}.dump();
    }

    RasterValueOptions formulaOptions(const std::string &formula)
    {
        RasterValueOptions options;
        options.formula = formula;
        return options;
    }

    // The message of the RefusedArgumentException of call, empty when it throws none
    std::string refusalOf(const std::function<void()> &call)
    {
        try
        {
            call();
        }
        catch (const RefusedArgumentException &e)
        {
            return e.what();
        }
        return "";
    }

    // 15000 at the left edge, 10 more per column: raw counts of a thermal sensor, without a calibration
    UInt16Spec rawCountsSpec()
    {
        UInt16Spec spec;
        spec.description = "thermal";
        spec.value = [](int x, int) { return 15000 + 10 * x; };
        return spec;
    }

    // ---------------------------------------------------------------- raw counts without a calibration

    // The profile follows the rule of the point and the area: the raw counts have no unit (they were labelled in degrees
    // Celsius) and a thermal formula is refused (it was accepted, and gave the raw counts in degrees)
    TEST(ThermalRawCounts, theProfileOfUncalibratedRawCountsHasNoUnitAndRefusesAThermalFormula)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeUInt16Raster(ta.getPath("raw.tif"), rawCountsSpec()).string();

        const json info = json::parse(getRasterValueInfoJson(path));
        ASSERT_TRUE(info["isThermal"].get<bool>());
        EXPECT_EQ(info["unit"], "");

        const json point = json::parse(getRasterPointJson(path, 50, 0));
        EXPECT_EQ(point["unit"], "");
        EXPECT_EQ(point["value"], 15500);

        const json profile = json::parse(getRasterProfileJson(path, rowLine(10, 20), 11));
        EXPECT_TRUE(profile["isThermal"].get<bool>());
        EXPECT_EQ(profile["unit"], "");
        EXPECT_NEAR(profile["min"].get<double>(), 15100.0, 1e-1);
        EXPECT_NEAR(profile["max"].get<double>(), 15200.0, 1e-1);

        // The refusal of the point and of the profile: no calibration, the values are raw counts (not a DJI R-JPEG)
        for (const std::string &message :
             {refusalOf([&] { getRasterPointJson(path, 50, 0, formulaOptions("CELSIUS")); }),
              refusalOf([&] { getRasterProfileJson(path, rowLine(10, 20), 11, formulaOptions("CELSIUS")); }),
              refusalOf([&] { getRasterProfileJson(path, rowLine(10, 20), 11, formulaOptions("KELVIN")); })})
        {
            EXPECT_NE(message.find("No thermal calibration"), std::string::npos) << message;
            EXPECT_NE(message.find("raw sensor counts"), std::string::npos) << message;
            EXPECT_NE(message.find("no Planck constants"), std::string::npos) << message;
            EXPECT_EQ(message.find("DJI"), std::string::npos) << message;
        }
        // A formula that is not thermal, or does not exist, is refused as on any raster
        EXPECT_NE(refusalOf([&] { getRasterProfileJson(path, rowLine(10, 20), 11, formulaOptions("NOPE")); })
                      .find("Unknown formula"),
                  std::string::npos);
        EXPECT_NE(refusalOf([&] { getRasterProfileJson(path, rowLine(10, 20), 11, formulaOptions("NDVI")); }), "");
    }

    // A raw count the calibration has no temperature for has no value along a profile: its samples are null and stay
    // out of the statistics
    TEST(ThermalRawCounts, aRawCountWithoutATemperatureHasNoValueInAProfile)
    {
        TestArea ta(TEST_NAME, true);
        UInt16Spec spec;
        // 1000 (no temperature) left of the column 30, 16204 (12.6860 C) from it on
        spec.value = [](int x, int) { return x < 30 ? 1000 : 16204; };
        spec.xmp = "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>"
                   "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
                   "<rdf:Description rdf:about='' xmlns:FLIR='http://ns.flir.com/xmp/1.0/' FLIR:PlanckR1='21106.77'"
                   " FLIR:PlanckB='1501' FLIR:PlanckF='1' FLIR:PlanckO='-7340' FLIR:PlanckR2='0.012545258'/>"
                   "</rdf:RDF></x:xmpmeta><?xpacket end='w'?>";
        const std::string path = makeUInt16Raster(ta.getPath("flir.tif"), spec).string();

        const json point = json::parse(getRasterPointJson(path, 50, 0));
        ASSERT_EQ(point["unit"], kCelsius) << "the raster must be calibrated by its XMP";
        EXPECT_NEAR(point["value"].get<double>(), 12.6860, 0.01);

        const json profile = json::parse(getRasterProfileJson(path, rowLine(20, 40), 21));
        EXPECT_EQ(profile["unit"], kCelsius);
        const json &samples = profile["samples"];
        ASSERT_EQ(samples.size(), 21u);
        EXPECT_TRUE(samples[0]["value"].is_null());
        EXPECT_TRUE(samples[5]["value"].is_null());
        ASSERT_FALSE(samples[20]["value"].is_null());
        EXPECT_NEAR(samples[20]["value"].get<double>(), 12.6860, 0.01);
        int valid = 0;
        for (const auto &sample : samples)
        {
            if (sample["value"].is_null())
                continue;
            EXPECT_NEAR(sample["value"].get<double>(), 12.6860, 0.01);
            valid++;
        }
        EXPECT_EQ(profile["validCount"], valid);
        EXPECT_GE(valid, 10);
        EXPECT_LE(valid, 11);
        EXPECT_NEAR(profile["min"].get<double>(), 12.6860, 0.01);
        EXPECT_NEAR(profile["max"].get<double>(), 12.6860, 0.01);
    }

    // ---------------------------------------------------------------- physical values (a scale and an offset)

    // A UInt16 band with the scale and the offset of Landsat holds physical values: it is no raw counts, it has the
    // unit of a thermal raster and takes the thermal formulas, at a point, in an area, along a profile and in the info
    TEST(ThermalRawCounts, aUInt16BandWithAScaleAndAnOffsetHoldsPhysicalValuesNotRawCounts)
    {
        TestArea ta(TEST_NAME, true);
        UInt16Spec spec;
        spec.landsat = true;
        spec.value = [](int x, int) { return 40000 + 100 * x; };
        const std::string path = makeUInt16Raster(ta.getPath("landsat.tif"), spec).string();

        const json info = json::parse(getRasterValueInfoJson(path));
        ASSERT_TRUE(info["isThermal"].get<bool>());
        EXPECT_NE(info["unit"], "");
        EXPECT_NEAR(info["valueMin"].get<double>(), 285.7208, 1e-3);
        EXPECT_NEAR(info["valueMax"].get<double>(), 319.5592, 1e-3);
        EXPECT_FALSE(info.contains("calibration"));

        const json point = json::parse(getRasterPointJson(path, 50, 0));
        EXPECT_NE(point["unit"], "");
        EXPECT_NEAR(point["value"].get<double>(), 302.8109, 1e-3);
        EXPECT_EQ(point["rawValue"], 45000);
        EXPECT_NO_THROW(getRasterPointJson(path, 50, 0, formulaOptions("KELVIN")));
        EXPECT_NO_THROW(getRasterAreaStatsJson(path, 40, 0, 59, 9, formulaOptions("CELSIUS")));
        EXPECT_NO_THROW(getRasterValueInfoJson(path, formulaOptions("FAHRENHEIT")));

        const json profile = json::parse(getRasterProfileJson(path, rowLine(10, 20), 11));
        EXPECT_NE(profile["unit"], "");
        EXPECT_NEAR(profile["min"].get<double>(), 289.1388, 1e-2);
        EXPECT_NEAR(profile["max"].get<double>(), 292.5568, 1e-2);
        EXPECT_NO_THROW(getRasterProfileJson(path, rowLine(10, 20), 11, formulaOptions("CELSIUS")));
    }
} // namespace
