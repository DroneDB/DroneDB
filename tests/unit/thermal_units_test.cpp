/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Thermal units converted in one place (U-202: FR-XC-24, FR-RAS-03, FR-RAS-06, D-022, AC-011). A raster in K, in
// degrees Celsius or in degrees Fahrenheit gives its values in the unit of the thermal formula (CELSIUS, KELVIN,
// FAHRENHEIT) everywhere: value info, point, area, profile, contours, tiles, thumbnails and exports. The expected
// numbers are literals (273.15 K = 0 C, 0 C = 32 F), never computed by the code under test.

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"

#include "contour.h"
#include "ddb.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gdaltiler.h"
#include "json.h"
#include "raster_analysis.h"
#include "raster_profile.h"
#include "raster_test_utils.h"
#include "thermal.h"
#include "tilerhelper.h"
#include "vegetation.h"
#include "vis_render.h"

namespace
{
    using namespace ddb;

    const char *const kCelsius = "\xC2\xB0" "C";
    const char *const kFahrenheit = "\xC2\xB0" "F";

    // A one-band Float32 GeoTIFF whose pixel (x, y) holds value(x, y), with the unit type `unit`
    struct ThermalSpec
    {
        int epsg = 3857;
        double originX = 1670000.0;
        double originY = 5645600.0;
        double pixel = 4.0;
        int width = 100;
        int height = 100;
        std::string unit;
        std::function<double(int, int)> value;
    };

    fs::path makeThermalRaster(const fs::path &path, const ThermalSpec &spec)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        GDALDatasetH hDs = GDALCreate(drv, path.string().c_str(), spec.width, spec.height, 1, GDT_Float32, nullptr);
        if (!hDs)
            throw std::runtime_error("Cannot create the test raster");
        const double gt[6] = {spec.originX, spec.pixel, 0.0, spec.originY, 0.0, -spec.pixel};
        GDALSetGeoTransform(hDs, const_cast<double *>(gt));
        OGRSpatialReference srs;
        srs.importFromEPSG(spec.epsg);
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        char *wkt = nullptr;
        srs.exportToWkt(&wkt);
        GDALSetProjection(hDs, wkt);
        CPLFree(wkt);

        GDALRasterBandH band = GDALGetRasterBand(hDs, 1);
        std::vector<float> row(static_cast<size_t>(spec.width));
        for (int y = 0; y < spec.height; y++)
        {
            for (int x = 0; x < spec.width; x++)
                row[static_cast<size_t>(x)] = static_cast<float>(spec.value(x, y));
            EXPECT_EQ(GDALRasterIO(band, GF_Write, 0, y, spec.width, 1, row.data(), spec.width, 1, GDT_Float32, 0, 0),
                      CE_None);
        }
        if (!spec.unit.empty())
            GDALSetRasterUnitType(band, spec.unit.c_str());
        GDALFlushCache(hDs);
        GDALClose(hDs);
        return path;
    }

    // 284.0 K at the left edge, 0.2 K more per column: 284.0 K (10.85 C, 51.53 F) to 303.8 K (30.65 C, 87.17 F)
    ThermalSpec kelvinSpec()
    {
        ThermalSpec spec;
        spec.unit = "K";
        spec.value = [](int x, int) { return 284.0 + 0.2 * x; };
        return spec;
    }

    // 10.0 C at the left edge, 0.2 C more per column: 10.0 C (283.15 K, 50 F) to 29.8 C (302.95 K, 85.64 F)
    ThermalSpec celsiusSpec()
    {
        ThermalSpec spec;
        spec.unit = "Celsius";
        spec.value = [](int x, int) { return 10.0 + 0.2 * x; };
        return spec;
    }

    RasterValueOptions formulaOptions(const std::string &formula)
    {
        RasterValueOptions options;
        options.formula = formula;
        return options;
    }

    // ---------------------------------------------------------------- the unit of a band and the conversions

    TEST(ThermalUnits, theUnitOfABandIsNormalisedOnce)
    {
        for (const char *kelvin : {"K", "k", "Kelvin", "kelvin", "degK", "\xC2\xB0" "K", " K "})
            EXPECT_EQ(parseTemperatureUnit(kelvin), TemperatureUnit::Kelvin) << kelvin;
        for (const char *celsius : {"C", "Celsius", "celsius", "degC", "\xC2\xB0" "C"})
            EXPECT_EQ(parseTemperatureUnit(celsius), TemperatureUnit::Celsius) << celsius;
        for (const char *fahrenheit : {"F", "Fahrenheit", "degF", "\xC2\xB0" "F"})
            EXPECT_EQ(parseTemperatureUnit(fahrenheit), TemperatureUnit::Fahrenheit) << fahrenheit;
        for (const char *other : {"", "m", "meters", "ft", "dN", "count"})
            EXPECT_FALSE(parseTemperatureUnit(other).has_value()) << other;

        // A band that names no temperature unit is in degrees Celsius (the unit of a thermal raster that says none)
        EXPECT_EQ(bandTemperatureUnit(""), TemperatureUnit::Celsius);
        EXPECT_EQ(bandTemperatureUnit("dN"), TemperatureUnit::Celsius);
        EXPECT_EQ(bandTemperatureUnit("K"), TemperatureUnit::Kelvin);

        EXPECT_STREQ(temperatureUnitSymbol(TemperatureUnit::Celsius), kCelsius);
        EXPECT_STREQ(temperatureUnitSymbol(TemperatureUnit::Kelvin), "K");
        EXPECT_STREQ(temperatureUnitSymbol(TemperatureUnit::Fahrenheit), kFahrenheit);
    }

    TEST(ThermalUnits, theConversionsAreThoseOfThePhysics)
    {
        const auto convert = [](TemperatureUnit from, TemperatureUnit to, double v) {
            return temperatureConversion(from, to).apply(v);
        };
        EXPECT_NEAR(convert(TemperatureUnit::Kelvin, TemperatureUnit::Celsius, 273.15), 0.0, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Kelvin, TemperatureUnit::Celsius, 284.6), 11.45, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Celsius, TemperatureUnit::Kelvin, 0.0), 273.15, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Celsius, TemperatureUnit::Fahrenheit, 0.0), 32.0, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Celsius, TemperatureUnit::Fahrenheit, 100.0), 212.0, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Fahrenheit, TemperatureUnit::Celsius, -40.0), -40.0, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Fahrenheit, TemperatureUnit::Celsius, 212.0), 100.0, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Kelvin, TemperatureUnit::Fahrenheit, 373.15), 212.0, 1e-9);
        EXPECT_NEAR(convert(TemperatureUnit::Fahrenheit, TemperatureUnit::Kelvin, 32.0), 273.15, 1e-9);
        EXPECT_TRUE(temperatureConversion(TemperatureUnit::Kelvin, TemperatureUnit::Kelvin).isIdentity());
    }

    TEST(ThermalUnits, aThermalFormulaNamesItsUnitAndNoIndexDoes)
    {
        EXPECT_EQ(thermalFormulaUnit("CELSIUS"), TemperatureUnit::Celsius);
        EXPECT_EQ(thermalFormulaUnit("KELVIN"), TemperatureUnit::Kelvin);
        EXPECT_EQ(thermalFormulaUnit("FAHRENHEIT"), TemperatureUnit::Fahrenheit);
        EXPECT_FALSE(thermalFormulaUnit("NDVI").has_value());
        EXPECT_FALSE(thermalFormulaUnit("celsius").has_value());
        // The engine lists the new formula next to the two of 2.2
        const VegetationFormula *fahrenheit = VegetationEngine::instance().getFormula("FAHRENHEIT");
        ASSERT_NE(fahrenheit, nullptr);
        EXPECT_EQ(fahrenheit->requiredBands, "T");

        const ThermalFormulaConversion c = thermalFormulaConversion("K", "CELSIUS");
        EXPECT_EQ(c.unit, kCelsius);
        EXPECT_NEAR(c.conversion.apply(300.0), 26.85, 1e-9);
        EXPECT_THROW(thermalFormulaConversion("K", "NDVI"), RefusedArgumentException);
        EXPECT_THROW(thermalFormulaConversion("K", "NOPE"), RefusedArgumentException);
        try
        {
            thermalFormulaConversion("K", "NOPE");
        }
        catch (const RefusedArgumentException &e)
        {
            EXPECT_STREQ(e.what(), "Unknown formula: NOPE");
        }
    }

    // CELSIUS over K subtracts 273.15, KELVIN over K keeps it, FAHRENHEIT converts; over degrees Celsius CELSIUS is
    // the identity and KELVIN adds 273.15 (the 2.2 behaviour); over degrees Fahrenheit CELSIUS converts down
    TEST(ThermalUnits, theFormulasConvertFromTheUnitOfTheBand)
    {
        const VegetationEngine &ve = VegetationEngine::instance();
        BandFilter filter;
        filter.T = 0;
        std::vector<float> band = {284.6f, 273.15f, -9999.0f};
        const std::vector<float *> bands = {band.data()};

        const auto compute = [&](const char *id, TemperatureUnit unit) {
            std::vector<float> out(band.size());
            ve.applyFormula(*ve.getFormula(id), filter, bands, out.data(), band.size(), -9999.0f, unit);
            return out;
        };

        const auto celsiusOfKelvin = compute("CELSIUS", TemperatureUnit::Kelvin);
        EXPECT_NEAR(celsiusOfKelvin[0], 11.45f, 1e-4);
        EXPECT_NEAR(celsiusOfKelvin[1], 0.0f, 1e-4);
        EXPECT_EQ(celsiusOfKelvin[2], -9999.0f); // no value stays no value

        const auto kelvinOfKelvin = compute("KELVIN", TemperatureUnit::Kelvin);
        EXPECT_FLOAT_EQ(kelvinOfKelvin[0], 284.6f);
        EXPECT_FLOAT_EQ(kelvinOfKelvin[1], 273.15f);

        const auto fahrenheitOfKelvin = compute("FAHRENHEIT", TemperatureUnit::Kelvin);
        EXPECT_NEAR(fahrenheitOfKelvin[1], 32.0f, 1e-3);
        EXPECT_NEAR(fahrenheitOfKelvin[0], 52.61f, 1e-3);

        band = {284.6f, 0.0f, -9999.0f};
        const auto celsiusOfCelsius = compute("CELSIUS", TemperatureUnit::Celsius);
        EXPECT_FLOAT_EQ(celsiusOfCelsius[0], 284.6f);
        const auto kelvinOfCelsius = compute("KELVIN", TemperatureUnit::Celsius);
        EXPECT_NEAR(kelvinOfCelsius[1], 273.15f, 1e-4);
        const auto fahrenheitOfCelsius = compute("FAHRENHEIT", TemperatureUnit::Celsius);
        EXPECT_NEAR(fahrenheitOfCelsius[1], 32.0f, 1e-4);

        band = {212.0f, -40.0f, -9999.0f};
        const auto celsiusOfFahrenheit = compute("CELSIUS", TemperatureUnit::Fahrenheit);
        EXPECT_NEAR(celsiusOfFahrenheit[0], 100.0f, 1e-3);
        EXPECT_NEAR(celsiusOfFahrenheit[1], -40.0f, 1e-3);

        // Without a unit, the band is in degrees Celsius: the 2.2 result
        std::vector<float> out(band.size());
        ve.applyFormula(*ve.getFormula("KELVIN"), filter, bands, out.data(), band.size(), -9999.0f);
        EXPECT_NEAR(out[1], 233.15f, 1e-4);
    }

    // ---------------------------------------------------------------- value info (FR-RAS-03, AC-011)

    TEST(ThermalUnits, theValueInfoOfAKelvinRasterIsInKelvinByDefaultAndConvertsOnRequest)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();

        // The band unit by default: no 273.15 is added or removed
        const json plain = json::parse(getRasterValueInfoJson(path));
        EXPECT_EQ(plain["unit"], "K");
        EXPECT_NEAR(plain["valueMin"].get<double>(), 284.0, 1e-4);
        EXPECT_NEAR(plain["valueMax"].get<double>(), 303.8, 1e-4);
        EXPECT_NEAR(plain["stretch"][0]["min"].get<double>(), 284.0, 1e-4);
        EXPECT_NEAR(plain["stretch"][0]["max"].get<double>(), 303.8, 1e-4);

        const json kelvin = json::parse(getRasterValueInfoJson(path, formulaOptions("KELVIN")));
        EXPECT_EQ(kelvin["unit"], "K");
        EXPECT_NEAR(kelvin["valueMin"].get<double>(), 284.0, 1e-4);
        EXPECT_NEAR(kelvin["valueMax"].get<double>(), 303.8, 1e-4);

        const json celsius = json::parse(getRasterValueInfoJson(path, formulaOptions("CELSIUS")));
        EXPECT_EQ(celsius["unit"], kCelsius);
        EXPECT_NEAR(celsius["valueMin"].get<double>(), 10.85, 1e-4);
        EXPECT_NEAR(celsius["valueMax"].get<double>(), 30.65, 1e-4);
        ASSERT_EQ(celsius["stretch"].size(), 1u);
        EXPECT_NEAR(celsius["stretch"][0]["min"].get<double>(), 10.85, 1e-4);
        EXPECT_NEAR(celsius["stretch"][0]["max"].get<double>(), 30.65, 1e-4);

        const json fahrenheit = json::parse(getRasterValueInfoJson(path, formulaOptions("FAHRENHEIT")));
        EXPECT_EQ(fahrenheit["unit"], kFahrenheit);
        EXPECT_NEAR(fahrenheit["valueMin"].get<double>(), 51.53, 1e-3);
        EXPECT_NEAR(fahrenheit["valueMax"].get<double>(), 87.17, 1e-3);
        EXPECT_NEAR(fahrenheit["stretch"][0]["max"].get<double>(), 87.17, 1e-3);
    }

    TEST(ThermalUnits, theValueInfoOfACelsiusRasterConvertsToKelvinAndFahrenheit)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("celsius.tif"), celsiusSpec()).string();

        const json plain = json::parse(getRasterValueInfoJson(path));
        EXPECT_EQ(plain["unit"], "Celsius");
        EXPECT_NEAR(plain["valueMin"].get<double>(), 10.0, 1e-4);

        const json celsius = json::parse(getRasterValueInfoJson(path, formulaOptions("CELSIUS")));
        EXPECT_EQ(celsius["unit"], kCelsius);
        EXPECT_NEAR(celsius["valueMax"].get<double>(), 29.8, 1e-4);

        const json kelvin = json::parse(getRasterValueInfoJson(path, formulaOptions("KELVIN")));
        EXPECT_EQ(kelvin["unit"], "K");
        EXPECT_NEAR(kelvin["valueMin"].get<double>(), 283.15, 1e-4);
        EXPECT_NEAR(kelvin["valueMax"].get<double>(), 302.95, 1e-4);
        EXPECT_NEAR(kelvin["stretch"][0]["min"].get<double>(), 283.15, 1e-4);

        const json fahrenheit = json::parse(getRasterValueInfoJson(path, formulaOptions("FAHRENHEIT")));
        EXPECT_EQ(fahrenheit["unit"], kFahrenheit);
        EXPECT_NEAR(fahrenheit["valueMin"].get<double>(), 50.0, 1e-3);
        EXPECT_NEAR(fahrenheit["valueMax"].get<double>(), 85.64, 1e-3);
    }

    TEST(ThermalUnits, aRasterInFahrenheitConvertsDown)
    {
        TestArea ta(TEST_NAME, true);
        ThermalSpec spec;
        spec.unit = "degF";
        spec.value = [](int x, int) { return 50.0 + x * 0.5; }; // 50 F (10 C) to 99.5 F (37.5 C)
        const std::string path = makeThermalRaster(ta.getPath("fahrenheit.tif"), spec).string();

        const json celsius = json::parse(getRasterValueInfoJson(path, formulaOptions("CELSIUS")));
        EXPECT_NEAR(celsius["valueMin"].get<double>(), 10.0, 1e-4);
        EXPECT_NEAR(celsius["valueMax"].get<double>(), 37.5, 1e-4);
        const json fahrenheit = json::parse(getRasterValueInfoJson(path, formulaOptions("FAHRENHEIT")));
        EXPECT_EQ(fahrenheit["unit"], kFahrenheit);
        EXPECT_NEAR(fahrenheit["valueMax"].get<double>(), 99.5, 1e-4);
    }

    // An index is no unit: the value info keeps its meaning (the stretch of the index), and a formula that does not
    // exist is still refused by the values of a point
    TEST(ThermalUnits, aFormulaThatIsNotThermalIsRefusedAtAPoint)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();
        EXPECT_THROW(getRasterPointJson(path, 10, 10, formulaOptions("NDVI")), RefusedArgumentException);
        EXPECT_THROW(getRasterPointJson(path, 10, 10, formulaOptions("NOPE")), RefusedArgumentException);
        EXPECT_THROW(getRasterProfileJson(path, R"({"type":"LineString","coordinates":[[15.0,45.0],[15.001,45.0]]})", 4,
                                          formulaOptions("NDVI")),
                     RefusedArgumentException);
        ContourOptions contours;
        contours.interval = 5.0;
        contours.formula = "NDVI";
        EXPECT_THROW(generateContoursJson(path, contours), RefusedArgumentException);
    }

    // ---------------------------------------------------------------- point, area, profile, contours

    // The pixel (50, 0) holds 294.0 K = 20.85 C = 69.53 F; the area (40, 0)-(59, 9) holds 292.0 to 295.8 K (mean 293.9)
    TEST(ThermalUnits, aPointAndAnAreaOfAKelvinRasterAreInTheSelectedUnit)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();

        const json plain = json::parse(getRasterPointJson(path, 50, 0));
        EXPECT_EQ(plain["unit"], "K");
        EXPECT_NEAR(plain["value"].get<double>(), 294.0, 1e-3);

        const json celsius = json::parse(getRasterPointJson(path, 50, 0, formulaOptions("CELSIUS")));
        EXPECT_EQ(celsius["unit"], kCelsius);
        EXPECT_NEAR(celsius["value"].get<double>(), 20.85, 1e-3);

        const json fahrenheit = json::parse(getRasterPointJson(path, 50, 0, formulaOptions("FAHRENHEIT")));
        EXPECT_EQ(fahrenheit["unit"], kFahrenheit);
        EXPECT_NEAR(fahrenheit["value"].get<double>(), 69.53, 1e-3);

        const json area = json::parse(getRasterAreaStatsJson(path, 40, 0, 59, 9, formulaOptions("CELSIUS")));
        EXPECT_EQ(area["unit"], kCelsius);
        EXPECT_NEAR(area["min"].get<double>(), 18.85, 1e-3);
        EXPECT_NEAR(area["max"].get<double>(), 22.65, 1e-3);
        EXPECT_NEAR(area["mean"].get<double>(), 20.75, 1e-3);
    }

    // The profile along the first row from column 10 to column 20 of a Web Mercator raster: 286.0 to 288.0 K
    TEST(ThermalUnits, aProfileOfAKelvinRasterIsInTheSelectedUnit)
    {
        TestArea ta(TEST_NAME, true);
        const ThermalSpec spec = kelvinSpec();
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), spec).string();

        const auto wgs84 = [&](int col) {
            double x = spec.originX + (col + 0.5) * spec.pixel;
            double y = spec.originY - 50.5 * spec.pixel;
            OGRSpatialReference from, to;
            from.importFromEPSG(spec.epsg);
            to.importFromEPSG(4326);
            from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&from, &to));
            ct->Transform(1, &x, &y);
            return std::make_pair(x, y);
        };
        const auto a = wgs84(10);
        const auto b = wgs84(20);
        const json line = {{"type", "LineString"}, {"coordinates", {{a.first, a.second}, {b.first, b.second}}}};

        const json plain = json::parse(getRasterProfileJson(path, line.dump(), 11));
        EXPECT_EQ(plain["unit"], "K");
        EXPECT_NEAR(plain["min"].get<double>(), 286.0, 1e-2);
        EXPECT_NEAR(plain["max"].get<double>(), 288.0, 1e-2);

        const json celsius = json::parse(getRasterProfileJson(path, line.dump(), 11, formulaOptions("CELSIUS")));
        EXPECT_EQ(celsius["unit"], kCelsius);
        EXPECT_NEAR(celsius["min"].get<double>(), 12.85, 1e-2);
        EXPECT_NEAR(celsius["max"].get<double>(), 14.85, 1e-2);
        ASSERT_FALSE(celsius["samples"][0]["value"].is_null());
        EXPECT_NEAR(celsius["samples"][0]["value"].get<double>(), 12.85, 1e-2);

        const json fahrenheit = json::parse(getRasterProfileJson(path, line.dump(), 11, formulaOptions("FAHRENHEIT")));
        EXPECT_EQ(fahrenheit["unit"], kFahrenheit);
        EXPECT_NEAR(fahrenheit["min"].get<double>(), 55.13, 1e-1);
        EXPECT_NEAR(fahrenheit["max"].get<double>(), 58.73, 1e-1);
    }

    // 284.0..303.8 K is 10.85..30.65 C: the levels of a 5 C interval are 15, 20, 25 and 30 C, whatever the interval
    // in K would have put on 284, 289, ...
    TEST(ThermalUnits, theContourLevelsOfAKelvinRasterAreInTheSelectedUnit)
    {
        TestArea ta(TEST_NAME, true);
        ThermalSpec spec = kelvinSpec();
        spec.epsg = 4326;
        spec.originX = 10.0;
        spec.originY = 45.0;
        spec.pixel = 0.0001;
        const std::string path = makeThermalRaster(ta.getPath("kelvin-geo.tif"), spec).string();

        ContourOptions options;
        options.interval = 5.0;
        options.formula = "CELSIUS";
        const json celsius = json::parse(generateContoursJson(path, options));
        EXPECT_EQ(celsius["unit"], kCelsius);
        EXPECT_NEAR(celsius["rasterMin"].get<double>(), 10.85, 1e-3);
        EXPECT_NEAR(celsius["rasterMax"].get<double>(), 30.65, 1e-3);
        EXPECT_NEAR(celsius["min"].get<double>(), 15.0, 1e-4);
        EXPECT_NEAR(celsius["max"].get<double>(), 30.0, 1e-4);
        EXPECT_EQ(celsius["featureCount"], 4);

        // Without a formula the levels are in the unit of the band: K, on multiples of 5 K
        ContourOptions kelvin;
        kelvin.interval = 5.0;
        const json plain = json::parse(generateContoursJson(path, kelvin));
        EXPECT_EQ(plain["unit"], "K");
        EXPECT_NEAR(plain["min"].get<double>(), 285.0, 1e-4);
        EXPECT_NEAR(plain["max"].get<double>(), 300.0, 1e-4);

        // The explicit limits are in the unit of the formula too
        ContourOptions limited = options;
        limited.minElev = 20.0;
        limited.maxElev = 25.0;
        const json limits = json::parse(generateContoursJson(path, limited));
        EXPECT_NEAR(limits["min"].get<double>(), 20.0, 1e-4);
        EXPECT_NEAR(limits["max"].get<double>(), 25.0, 1e-4);
        EXPECT_EQ(limits["featureCount"], 2);
    }

    // ---------------------------------------------------------------- tiles, thumbnails, exports (D-022)

    // Two renders of the same temperatures in other units differ by the rounding of a float at most: one grey level
    ::testing::AssertionResult sameImage(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
    {
        if (a.size() != b.size())
            return ::testing::AssertionFailure() << "sizes " << a.size() << " and " << b.size();
        size_t different = 0;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > 1)
                return ::testing::AssertionFailure() << "pixel " << i << ": " << static_cast<int>(a[i]) << " and "
                                                     << static_cast<int>(b[i]);
            different += a[i] != b[i];
        }
        if (different * 100 > a.size())
            return ::testing::AssertionFailure() << different << " of " << a.size() << " pixels differ";
        return ::testing::AssertionSuccess();
    }

    // A thumbnail of the raster with the formula, a colormap and a rescale, as a vector of the grey levels of its
    // opaque pixels
    std::vector<uint8_t> thumbnailOf(const std::string &path, const char *formula, const char *rescale,
                                     const DDBRasterOptions *options)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        const DDBErr err = DDBGenerateMemoryThumbnailEx2(path.c_str(), 100, nullptr, nullptr, formula, nullptr,
                                                         "grayscale", rescale, options, &buffer, &size);
        EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
        const auto image = testutils::decodeImage(buffer, static_cast<size_t>(size));
        DDBVSIFree(buffer);
        std::vector<uint8_t> grey;
        for (size_t i = 0; i < image.bands[0].size(); i++)
            if (!image.hasAlpha() || image.alpha()[i] > 0)
                grey.push_back(image.bands[0][i]);
        return grey;
    }

    // The same 10..30 C range given in K, F and C with the unit of the rescale gives the same image as the range in
    // the unit of the formula, and the image is a gradient: the temperatures of a K raster are read as K (before,
    // CELSIUS read 284..304 as degrees Celsius: a white image)
    TEST(ThermalUnits, aThumbnailOfAKelvinRasterIsStretchedInTheUnitOfTheFormula)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();

        const auto celsius = thumbnailOf(path, "CELSIUS", "10,30", nullptr);
        ASSERT_GT(celsius.size(), 1000u);
        const std::set<uint8_t> levels(celsius.begin(), celsius.end());
        EXPECT_GT(levels.size(), 30u);
        // 10.85 C at the left edge is dark, 30.65 C at the right edge saturates: the leftmost pixel of the middle row
        // is the darkest level and the rightmost the brightest
        EXPECT_LT(*levels.begin(), 60);
        EXPECT_EQ(*levels.rbegin(), 255);

        const auto kelvin = thumbnailOf(path, "KELVIN", "283.15,303.15", nullptr);
        EXPECT_TRUE(sameImage(kelvin, celsius));

        const auto fahrenheit = thumbnailOf(path, "FAHRENHEIT", "50,86", nullptr);
        EXPECT_TRUE(sameImage(fahrenheit, celsius));

        // The rescale given in another unit keeps its temperatures (D-022)
        DDBRasterOptions inKelvin = {};
        inKelvin.structSize = sizeof(inKelvin);
        inKelvin.rescaleUnit = "K";
        EXPECT_TRUE(sameImage(thumbnailOf(path, "CELSIUS", "283.15,303.15", &inKelvin), celsius));
        DDBRasterOptions inFahrenheit = {};
        inFahrenheit.structSize = sizeof(inFahrenheit);
        inFahrenheit.rescaleUnit = "degF";
        EXPECT_TRUE(sameImage(thumbnailOf(path, "CELSIUS", "50,86", &inFahrenheit), celsius));
        EXPECT_TRUE(sameImage(thumbnailOf(path, "KELVIN", "50,86", &inFahrenheit), celsius));

        // A rescale unit that is not a temperature unit is refused
        DDBRasterOptions bad = {};
        bad.structSize = sizeof(bad);
        bad.rescaleUnit = "meters";
        uint8_t *buffer = nullptr;
        int size = 0;
        EXPECT_EQ(DDBGenerateMemoryThumbnailEx2(path.c_str(), 100, nullptr, nullptr, "CELSIUS", nullptr, "grayscale",
                                                "10,30", &bad, &buffer, &size),
                  DDBERR_INVALIDARGS);
    }

    // A caller of 2.2 passes a struct that stops before rescaleUnit: its formula still counts and the rescale unit is
    // unset
    TEST(ThermalUnits, aStructOfTheOldSizeKeepsItsFormula)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();

        DDBRasterOptions options = {};
        options.structSize = static_cast<uint32_t>(offsetof(DDBRasterOptions, rescaleUnit));
        options.formula = "CELSIUS";
        char *output = nullptr;
        ASSERT_EQ(DDBGetRasterPointValuePixelEx(path.c_str(), 50, 0, &options, &output), DDBERR_NONE)
            << DDBGetLastError();
        const json point = json::parse(output);
        DDBFree(output);
        EXPECT_EQ(point["unit"], kCelsius);
        EXPECT_NEAR(point["value"].get<double>(), 20.85, 1e-3);

        // The full struct carries both
        DDBRasterOptions full = {};
        full.structSize = sizeof(full);
        full.formula = "FAHRENHEIT";
        ASSERT_EQ(DDBGetRasterValueInfoEx(path.c_str(), &full, &output), DDBERR_NONE) << DDBGetLastError();
        const json info = json::parse(output);
        DDBFree(output);
        EXPECT_EQ(info["unit"], kFahrenheit);
        EXPECT_NEAR(info["valueMax"].get<double>(), 87.17, 1e-3);

        // The contours of the C export take the formula of the options
        ASSERT_EQ(DDBGenerateContoursEx(path.c_str(), 5.0, 0, 0.0, std::nan(""), std::nan(""), 0.0, 1, &options,
                                        &output),
                  DDBERR_NONE)
            << DDBGetLastError();
        const json contours = json::parse(output);
        DDBFree(output);
        EXPECT_EQ(contours["unit"], kCelsius);
    }

    // The tiles come from the COG of the build, which keeps the unit of the band (D-021): a tile of a K raster is
    // stretched like its thumbnail, a gradient in the unit of the formula
    TEST(ThermalUnits, aTileOfAKelvinRasterIsStretchedInTheUnitOfTheFormula)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();

        int z = 0;
        BoundingBox<Projected2Di> b;
        {
            GDALTiler t(TilerHelper::toGeoTIFF(path).string(), "", 256);
            z = t.getMinMaxZ().max;
            b = t.getMinMaxCoordsForZ(z);
        }
        const auto tile = [&](const char *formula, const char *rescale, int x, int y,
                              const DDBRasterOptions *options) {
            uint8_t *buffer = nullptr;
            int size = 0;
            const DDBErr err = DDBMemoryTileEx2(path.c_str(), z, x, y, 256, false, false, nullptr, nullptr, nullptr,
                                                formula, nullptr, "grayscale", rescale, options, &buffer, &size);
            EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
            const auto image = testutils::decodeImage(buffer, static_cast<size_t>(size));
            DDBVSIFree(buffer);
            return image;
        };
        const auto grey = [](const testutils::DecodedImage &image) {
            std::vector<uint8_t> out;
            for (size_t i = 0; i < image.bands[0].size(); i++)
                if (!image.hasAlpha() || image.alpha()[i] > 0)
                    out.push_back(image.bands[0][i]);
            return out;
        };

        // The tile with the most pixels
        int bestX = b.min.x, bestY = b.min.y;
        size_t best = 0;
        for (int x = b.min.x; x <= b.max.x; x++)
            for (int y = b.min.y; y <= b.max.y; y++)
            {
                const size_t count = grey(tile("CELSIUS", "10,30", x, y, nullptr)).size();
                if (count > best)
                {
                    best = count;
                    bestX = x;
                    bestY = y;
                }
            }
        ASSERT_GT(best, 1000u);

        const auto celsius = grey(tile("CELSIUS", "10,30", bestX, bestY, nullptr));
        const std::set<uint8_t> levels(celsius.begin(), celsius.end());
        EXPECT_GT(levels.size(), 10u) << "a K raster read as degrees Celsius saturates the tile";
        EXPECT_LT(*levels.begin(), 150);

        EXPECT_TRUE(sameImage(grey(tile("KELVIN", "283.15,303.15", bestX, bestY, nullptr)), celsius));
        EXPECT_TRUE(sameImage(grey(tile("FAHRENHEIT", "50,86", bestX, bestY, nullptr)), celsius));
        DDBRasterOptions inKelvin = {};
        inKelvin.structSize = sizeof(inKelvin);
        inKelvin.rescaleUnit = "K";
        EXPECT_TRUE(sameImage(grey(tile("CELSIUS", "283.15,303.15", bestX, bestY, &inKelvin)), celsius));
    }

    // The export of the styled raster is rendered as the thumbnail is: the stretch is that of the unit of the formula
    TEST(ThermalUnits, anExportOfAKelvinRasterIsStretchedInTheUnitOfTheFormula)
    {
        TestArea ta(TEST_NAME, true);
        const std::string path = makeThermalRaster(ta.getPath("kelvin.tif"), kelvinSpec()).string();

        const auto exported = [&](const char *formula, const char *rescale, const DDBRasterOptions *options,
                                  const char *name) {
            const std::string out = ta.getPath(name).string();
            const DDBErr err = DDBExportRaster3(path.c_str(), out.c_str(), nullptr, nullptr, formula, nullptr,
                                                "grayscale", rescale, 0, nullptr, nullptr, options);
            EXPECT_EQ(err, DDBERR_NONE) << DDBGetLastError();
            GDALDatasetH hDs = GDALOpen(out.c_str(), GA_ReadOnly);
            std::set<int> levels;
            if (hDs != nullptr)
            {
                const int w = GDALGetRasterXSize(hDs), h = GDALGetRasterYSize(hDs);
                std::vector<uint8_t> row(static_cast<size_t>(w));
                GDALRasterBandH red = GDALGetRasterBand(hDs, 1);
                for (int y = 0; y < h; y += std::max(1, h / 20))
                {
                    EXPECT_EQ(GDALRasterIO(red, GF_Read, 0, y, w, 1, row.data(), w, 1, GDT_Byte, 0, 0), CE_None);
                    levels.insert(row.begin(), row.end());
                }
                GDALClose(hDs);
            }
            return levels;
        };

        const auto celsius = exported("CELSIUS", "10,30", nullptr, "celsius.tif");
        EXPECT_GT(celsius.size(), 20u);
        DDBRasterOptions inKelvin = {};
        inKelvin.structSize = sizeof(inKelvin);
        inKelvin.rescaleUnit = "K";
        EXPECT_EQ(exported("CELSIUS", "283.15,303.15", &inKelvin, "celsius-from-k.tif"), celsius);
    }

    TEST(ThermalUnits, convertingARescaleKeepsItsTemperatures)
    {
        EXPECT_EQ(convertRescale("10,30", TemperatureUnit::Celsius, TemperatureUnit::Kelvin), "283.15,303.15");
        EXPECT_EQ(convertRescale("273.15,373.15", TemperatureUnit::Kelvin, TemperatureUnit::Celsius), "0,100");
        EXPECT_EQ(convertRescale("0,100", TemperatureUnit::Celsius, TemperatureUnit::Fahrenheit), "32,212");
        EXPECT_EQ(convertRescale("32,212", TemperatureUnit::Fahrenheit, TemperatureUnit::Celsius), "0,100");
        EXPECT_THROW(convertRescale("10", TemperatureUnit::Celsius, TemperatureUnit::Kelvin), InvalidArgsException);
        EXPECT_THROW(convertRescale("a,b", TemperatureUnit::Celsius, TemperatureUnit::Kelvin), InvalidArgsException);
    }
}
