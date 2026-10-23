/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-06 golden outputs of the code that reads a coordinate reference system from a file: raster
// tiles, regions and point queries, and the extents, MVT pyramid and bbox queries of vector
// files, in the CRSs whose authority axis order is not x = east (EPSG:4258 latitude first,
// EPSG:3035 northing first) next to those that are. Nothing there may depend on the
// process-wide OGR_CT_FORCE_TRADITIONAL_GIS_ORDER setting.

#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "test.h"
#include "testarea.h"
#include "raster_test_utils.h"

#include "ddb.h"
#include "dq06_golden_support.h"
#include "entry.h"
#include "gdal_inc.h"
#include "gdaltiler.h"
#include "raster_region.h"
#include "vector.h"
#include "vector_query.h"

namespace
{
    using namespace ddb;

    struct CrsPlace
    {
        const char *name;
        int epsg;
        double originX; // upper-left corner in the CRS
        double originY;
        double pixel;
    };

    const std::vector<CrsPlace> kPlaces = {
        {"wgs84", 4326, 10.0, 45.05, 0.0004},
        {"etrs89", 4258, 10.0, 45.05, 0.0004},
        {"utm32", 32632, 578000.0, 4990000.0, 30.0},
        {"web", 3857, 1113000.0, 5627000.0, 40.0},
        {"laea", 3035, 4300000.0, 2510000.0, 30.0},
    };

    // x and y of a point given by longitude and latitude, in the CRS with the traditional axis
    // order set explicitly (the test does not depend on the process-wide setting)
    std::pair<double, double> fromLonLat(int epsg, double lon, double lat)
    {
        OGRSpatialReference wgs84, to;
        wgs84.importFromEPSG(4326);
        to.importFromEPSG(epsg);
        wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&wgs84, &to));
        if (!ct || !ct->Transform(1, &lon, &lat))
            throw std::runtime_error("Cannot transform a test point");
        return {lon, lat};
    }

    class Dq06Crs : public ::testing::Test
    {
    protected:
        void SetUp() override { DDBRegisterProcess(false); }
    };

    // ---- rasters ----------------------------------------------------------------------

    fs::path makeOrtho(const fs::path &path, const CrsPlace &place)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 100;
        spec.bands = 3;
        spec.type = GDT_Byte;
        spec.epsg = place.epsg;
        spec.originX = place.originX;
        spec.originY = place.originY;
        spec.pixelSize = place.pixel;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * (band + 2) + y * (5 - band) + 40 * band) % 256); };
        return testutils::createGeoRaster(path, spec);
    }

    std::string hashOf(const testutils::DecodedImage &img)
    {
        std::string all;
        for (const auto &plane : img.bands)
            all += dq06::fnv1a(plane.data(), plane.size());
        return dq06::fnv1a(all.data(), all.size());
    }

    json imageSummary(const std::vector<uint8_t> &bytes)
    {
        const testutils::DecodedImage img = testutils::decodeImage(bytes);
        json out = {{"width", img.width}, {"height", img.height}, {"bands", img.bands.size()}, {"pixels", hashOf(img)}};
        if (img.hasAlpha())
        {
            size_t transparent = 0;
            for (const uint8_t a : img.alpha())
                transparent += (a == 0);
            out["transparent"] = transparent;
        }
        return out;
    }

    TEST_F(Dq06Crs, tilesOfRastersInEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &place : kPlaces)
        {
            const fs::path ortho = makeOrtho(ta.getPath(std::string(place.name) + ".tif"), place);
            GDALTiler tiler(ortho.string(), "", 256, false);
            const auto zooms = tiler.getMinMaxZ();
            out[place.name]["zooms"] = {zooms.min, zooms.max};
            for (const int z : {zooms.min, zooms.max})
            {
                const auto bounds = tiler.getMinMaxCoordsForZ(z);
                json tiles = json::object();
                for (int tx = bounds.min.x; tx <= bounds.max.x; tx++)
                    for (int ty = bounds.min.y; ty <= bounds.max.y; ty++)
                    {
                        uint8_t *buffer = nullptr;
                        int size = 0;
                        const DDBErr err = DDBMemoryTile(ortho.string().c_str(), z, tx, ty, &buffer, &size, 256, false, true, "dq06");
                        const std::string key = std::to_string(tx) + "_" + std::to_string(ty);
                        if (err != DDBERR_NONE)
                        {
                            tiles[key] = {{"error", DDBGetLastError()}};
                            continue;
                        }
                        tiles[key] = imageSummary(testutils::takeBuffer(buffer, size));
                    }
                out[place.name]["z" + std::to_string(z)] = tiles;
            }
        }
        dq06::expectGolden("crs_tiles", out);
    }

    TEST_F(Dq06Crs, regionsAndPointsOfRastersInEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &place : kPlaces)
        {
            const fs::path ortho = makeOrtho(ta.getPath(std::string(place.name) + ".tif"), place);
            // The middle of the raster, as longitude and latitude
            OGRSpatialReference own, wgs84;
            own.importFromEPSG(place.epsg);
            wgs84.importFromEPSG(4326);
            own.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            std::unique_ptr<OGRCoordinateTransformation> toLonLat(OGRCreateCoordinateTransformation(&own, &wgs84));
            double lon = place.originX + 50 * place.pixel, lat = place.originY - 50 * place.pixel;
            ASSERT_TRUE(toLonLat && toLonLat->Transform(1, &lon, &lat));

            json region = json::object();
            for (const char *requested : {"EPSG:4326", "EPSG:4258", "EPSG:3857", "EPSG:3035", "EPSG:32632"})
            {
                const int epsg = std::stoi(std::string(requested).substr(5));
                const auto [cx, cy] = fromLonLat(epsg, lon, lat);
                const double half = epsg == 4326 || epsg == 4258 ? 0.004 : 400.0;
                const double bbox[4] = {cx - half, cy - half, cx + half, cy + half};
                uint8_t *buffer = nullptr;
                int size = 0;
                try
                {
                    renderRasterRegion(ortho.string(), bbox, requested, 48, 48, "image/png", &buffer, &size);
                    region[requested] = imageSummary(testutils::takeBuffer(buffer, size));
                }
                catch (const std::exception &e)
                {
                    region[requested] = {{"error", e.what()}};
                }
                // The point at the middle of the raster, asked in this CRS
                try
                {
                    region[std::string("point_") + requested] = json::parse(queryRasterPoint(ortho.string(), cx, cy, requested));
                }
                catch (const std::exception &e)
                {
                    region[std::string("point_") + requested] = {{"error", e.what()}};
                }
            }
            out[place.name] = region;
        }
        dq06::expectGolden("crs_regions_and_points", out);
    }

    // ---- vectors ----------------------------------------------------------------------

    struct VectorCase
    {
        const char *name;
        const char *driver;
        const char *extension;
        int epsg;
    };

    const std::vector<VectorCase> kVectors = {
        {"gpkg4326", "GPKG", "gpkg", 4326},   {"gpkg4258", "GPKG", "gpkg", 4258},
        {"gpkg3035", "GPKG", "gpkg", 3035},   {"gpkg3857", "GPKG", "gpkg", 3857},
        {"gpkg32632", "GPKG", "gpkg", 32632}, {"shp4258", "ESRI Shapefile", "shp", 4258},
        {"shp3035", "ESRI Shapefile", "shp", 3035}, {"fgb4258", "FlatGeobuf", "fgb", 4258},
        {"fgb3035", "FlatGeobuf", "fgb", 3035}, {"fgb3857", "FlatGeobuf", "fgb", 3857},
        {"geojson4326", "GeoJSON", "geojson", 4326},
    };

    // Two overlapping boxes given by longitude and latitude, written in the CRS of the layer
    void writeVector(const fs::path &path, const VectorCase &c)
    {
        GDALDriver *drv = GetGDALDriverManager()->GetDriverByName(c.driver);
        ASSERT_NE(drv, nullptr) << c.driver;
        std::unique_ptr<GDALDataset> ds(drv->Create(path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        ASSERT_NE(ds, nullptr) << path;
        OGRSpatialReference srs;
        srs.importFromEPSG(c.epsg);
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRLayer *layer = ds->CreateLayer("boxes", &srs, wkbPolygon, nullptr);
        ASSERT_NE(layer, nullptr);
        OGRFieldDefn nameField("name", OFTString);
        layer->CreateField(&nameField);
        const double boxes[2][4] = {{10.00, 45.00, 10.05, 45.03}, {10.02, 45.01, 10.08, 45.05}};
        for (int i = 0; i < 2; i++)
        {
            OGRLinearRing ring;
            const double corners[5][2] = {{boxes[i][0], boxes[i][1]}, {boxes[i][2], boxes[i][1]},
                                          {boxes[i][2], boxes[i][3]}, {boxes[i][0], boxes[i][3]},
                                          {boxes[i][0], boxes[i][1]}};
            for (const auto &corner : corners)
            {
                const auto [x, y] = fromLonLat(c.epsg, corner[0], corner[1]);
                ring.addPoint(x, y);
            }
            auto *poly = new OGRPolygon();
            poly->addRing(&ring);
            OGRFeature feature(layer->GetLayerDefn());
            feature.SetField("name", i == 0 ? "first" : "second");
            feature.SetGeometryDirectly(poly);
            ASSERT_EQ(layer->CreateFeature(&feature), OGRERR_NONE);
        }
    }

    TEST_F(Dq06Crs, vectorEntriesInEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &c : kVectors)
        {
            const fs::path file = ta.getPath(std::string(c.name) + "." + c.extension);
            writeVector(file, c);
            Entry e;
            try
            {
                parseEntry(file, ta.getFolder(), e, false);
                out[c.name] = {{"type", static_cast<int>(e.type)},
                               {"point", e.point_geom.toWkt()},
                               {"polygon", e.polygon_geom.toWkt()},
                               {"vector", e.properties.value("vector", json::object())}};
            }
            catch (const std::exception &ex)
            {
                out[c.name] = {{"error", ex.what()}};
            }
        }
        dq06::expectGolden("crs_vector_entries", out);
    }

    json layerExtents(const fs::path &gpkg)
    {
        json out = json::array();
        std::unique_ptr<GDALDataset> ds(
            static_cast<GDALDataset *>(GDALOpenEx(gpkg.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
        if (!ds)
            return {{"error", "cannot open"}};
        for (int i = 0; i < ds->GetLayerCount(); i++)
        {
            OGRLayer *layer = ds->GetLayer(i);
            OGREnvelope env;
            layer->GetExtent(&env, TRUE);
            const char *auth = layer->GetSpatialRef() != nullptr ? layer->GetSpatialRef()->GetAuthorityCode(nullptr) : nullptr;
            out.push_back({{"name", layer->GetName()},
                           {"features", layer->GetFeatureCount()},
                           {"srs", auth ? auth : ""},
                           {"extent", {env.MinX, env.MinY, env.MaxX, env.MaxY}}});
        }
        return out;
    }

    TEST_F(Dq06Crs, vectorBuildAndQueriesInEveryCrs)
    {
        TestArea ta(TEST_NAME, true);
        json out = json::object();
        for (const auto &c : kVectors)
        {
            const fs::path file = ta.getPath(std::string(c.name) + "." + c.extension);
            writeVector(file, c);
            const fs::path build = ta.getPath(std::string("build_") + c.name);
            try
            {
                buildVector(file.string(), build.string());
            }
            catch (const std::exception &e)
            {
                out[c.name] = {{"error", e.what()}};
                continue;
            }
            json one;
            const fs::path gpkg = build / "vec" / "source.gpkg";
            one["layers"] = layerExtents(gpkg);

            std::ifstream meta(build / "mvt" / "metadata.json");
            json metadata = json::parse(meta, nullptr, false);
            json kept = json::object();
            for (const char *key : {"bounds", "center", "minzoom", "maxzoom"})
                if (metadata.is_object() && metadata.contains(key))
                    kept[key] = metadata[key];
            one["mvt"] = kept;

            // Only the first box (the second starts at 10.02), asked for in several CRSs (x = longitude or easting)
            json queries = json::object();
            for (const int epsg : {4326, 4258, 3857, 3035, 32632})
            {
                const auto [x0, y0] = fromLonLat(epsg, 10.00, 45.00);
                const auto [x1, y1] = fromLonLat(epsg, 10.015, 45.01);
                ddb::VectorQuery q;
                q.bbox = {{std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)}};
                q.bboxSrs = "EPSG:" + std::to_string(epsg);
                json result;
                try
                {
                    const json fc = json::parse(queryVector(gpkg.string(), q));
                    json names = json::array();
                    for (const auto &f : fc["features"])
                        names.push_back(f["properties"]["name"]);
                    result = {{"count", fc["features"].size()}, {"names", names}};
                    if (!fc["features"].empty())
                        result["first"] = fc["features"][0]["geometry"];
                }
                catch (const std::exception &ex)
                {
                    result = {{"error", ex.what()}};
                }
                queries["EPSG:" + std::to_string(epsg)] = result;
            }
            one["queries"] = queries;
            out[c.name] = one;
        }
        dq06::expectGolden("crs_vector_build_and_queries", out);
    }
}
