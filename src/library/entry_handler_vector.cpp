/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Vector entries: GeoJSON, Shapefile, GeoPackage, KML, FlatGeobuf, CAD drawings (DXF/DWG), ...
// built into a GeoPackage copy plus an MVT pyramid (or the drawing-coordinate metadata of a CAD
// drawing without CRS).

#include <algorithm>
#include <limits>
#include <optional>

#include "build_layout.h"
#include "json.h"
#include "cad.h"
#include "coordstransformer.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gdal_raii.h"
#include "ogr_api.h"
#include "ogr_spatialref.h"
#include "vector.h"

namespace ddb {

namespace {

void parseVectorMetadata(const fs::path &path, Entry &entry)
{
    // Compute polygon_geom (WGS84) and properties.vector multi-layer metadata.
    // Best-effort: any failure leaves the entry indexed without geometry but
    // never aborts the parse (a corrupted vector file should not block ddb add).
    // Declared outside try: the CAD source outlives the dataset (a converted DXF is
    // deleted only once closed).
    std::optional<cad::CadSource> cadSource;
    try
    {
        // CAD drawings: a DWG is read through a temporary DXF, and the CRS
        // comes from the drawing (the same rule the build uses, cad.h).
        const bool isCad = cad::isCadFile(path);
        if (isCad)
            cadSource.emplace(path);
        const fs::path openPath = isCad ? cadSource->dxfPath() : path;

        const auto dataset = adoptDataset(GDALOpenEx(io::toUtf8(openPath).c_str(),
                                                    GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                    nullptr, nullptr, nullptr));
        if (!dataset)
        {
            LOGD << "Vector parse: GDAL cannot open " << io::toUtf8(path);
            return;
        }
        GDALDatasetH hDS = datasetHandle(dataset);
        SrsPtr cadSrs;

        json cadJson;
        if (isCad)
        {
            const auto info = cad::readDxfInfo(openPath);
            const auto crs = cad::resolveCadCrs(path, hDS, info);
            cadJson = {{"crs", {{"source", crs.source}, {"label", crs.label}}},
                       {"units", cad::insUnitsName(info.insUnits)}};
            if (cadSource->converted()) cadJson["converter"] = "dwg2dxf";
            entry.properties["georeferenced"] = crs.georeferenced;

            if (!crs.georeferenced)
            {
                // Local drawing: no footprint (nothing to place on a map); the
                // layer extents stay in drawing coordinates.
                auto layers = json::array();
                for (int i = 0; i < GDALDatasetGetLayerCount(hDS); i++)
                {
                    OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
                    if (hLayer == nullptr) continue;
                    const char *layerName = OGR_L_GetName(hLayer);
                    auto layerJson = json::object();
                    layerJson["name"] = layerName ? layerName : ("layer_" + std::to_string(i));
                    layerJson["geometryType"] = OGRGeometryTypeToName(OGR_L_GetGeomType(hLayer));
                    layerJson["featureCount"] = static_cast<int64_t>(OGR_L_GetFeatureCount(hLayer, FALSE));
                    layers.push_back(layerJson);
                }
                entry.properties["vector"] = {
                    {"driver", GDALGetDriverShortName(GDALGetDatasetDriver(hDS))},
                    {"layers", layers}};
                entry.properties["vector"].update(cadJson);
                if (crs.hasExtent)
                    entry.properties["vector"]["localExtent"] = {crs.extent.MinX, crs.extent.MinY,
                                                                 crs.extent.MaxX, crs.extent.MaxY};
                return;
            }

            if (!crs.srs.empty())
            {
                cadSrs = srsFromUserInput(crs.srs.c_str());
                if (!cadSrs)
                    throw GDALException("Invalid CAD coordinate system " + crs.label);
            }
        }

        // Lazy WGS84 SRS - only allocate when needed (first layer with SRS).
        SrsPtr wgs84;
        // Convex envelope in WGS84 across all layers.
        OGREnvelope totalEnv;
        totalEnv.MinX = totalEnv.MinY =  std::numeric_limits<double>::max();
        totalEnv.MaxX = totalEnv.MaxY = -std::numeric_limits<double>::max();
        bool haveEnv = false;

        const int layerCount = GDALDatasetGetLayerCount(hDS);
        auto layersArr = json::array();

        for (int i = 0; i < layerCount; i++)
        {
            OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
            if (hLayer == nullptr) continue;

            OGREnvelope env;
            if (OGR_L_GetExtent(hLayer, &env, TRUE) != OGRERR_NONE)
            {
                LOGD << "Vector parse: layer " << i << " has no extent";
                continue;
            }

            OGRSpatialReferenceH hSrs = OGR_L_GetSpatialRef(hLayer);
            if (hSrs == nullptr) hSrs = cadSrs.get(); // CAD layers carry no SRS
            OGREnvelope envWgs84 = env;

            if (hSrs != nullptr)
            {
                // Check if reprojection is needed (skip if already WGS84).
                const char *authCode = OSRGetAuthorityCode(hSrs, nullptr);
                const char *authName = OSRGetAuthorityName(hSrs, nullptr);
                bool isWgs84 = (authName && authCode &&
                                std::string(authName) == "EPSG" &&
                                std::string(authCode) == "4326");

                if (!isWgs84)
                {
                    if (!wgs84)
                    {
                        wgs84 = srsFromEpsg(4326);
                        if (!wgs84)
                        {
                            LOGD << "Vector parse: failed to import EPSG:4326";
                        }
                    }

                    if (wgs84)
                    {
                        const auto toWgs84 =
                            CoordsTransformer::tryCreate(*OGRSpatialReference::FromHandle(hSrs), srsOf(wgs84));
                        if (toWgs84)
                        {
                            // Transform all 4 corners (a single rectangle in src
                            // CRS can become a curved quadrilateral in WGS84).
                            double xs[4] = {env.MinX, env.MaxX, env.MaxX, env.MinX};
                            double ys[4] = {env.MinY, env.MinY, env.MaxY, env.MaxY};
                            if (toWgs84->tryTransform(4, xs, ys))
                            {
                                envWgs84.MinX = std::min({xs[0], xs[1], xs[2], xs[3]});
                                envWgs84.MaxX = std::max({xs[0], xs[1], xs[2], xs[3]});
                                envWgs84.MinY = std::min({ys[0], ys[1], ys[2], ys[3]});
                                envWgs84.MaxY = std::max({ys[0], ys[1], ys[2], ys[3]});
                            }
                            else
                            {
                                LOGD << "Vector parse: layer " << i << " transform failed";
                            }
                        }
                    }
                }
            }
            else
            {
                LOGD << "Vector parse: layer " << i << " has no SRS, assuming WGS84";
            }

            // Per-layer metadata
            auto layerJson = json::object();
            const char *layerName = OGR_L_GetName(hLayer);
            layerJson["name"] = layerName ? layerName : ("layer_" + std::to_string(i));

            OGRwkbGeometryType gType = OGR_L_GetGeomType(hLayer);
            layerJson["geometryType"] = OGRGeometryTypeToName(gType);

            GIntBig fc = OGR_L_GetFeatureCount(hLayer, FALSE);
            layerJson["featureCount"] = static_cast<int64_t>(fc);
            layerJson["extent"] = {envWgs84.MinX, envWgs84.MinY, envWgs84.MaxX, envWgs84.MaxY};

            layersArr.push_back(layerJson);

            // Accumulate global envelope
            if (!haveEnv)
            {
                totalEnv = envWgs84;
                haveEnv = true;
            }
            else
            {
                totalEnv.MinX = std::min(totalEnv.MinX, envWgs84.MinX);
                totalEnv.MinY = std::min(totalEnv.MinY, envWgs84.MinY);
                totalEnv.MaxX = std::max(totalEnv.MaxX, envWgs84.MaxX);
                totalEnv.MaxY = std::max(totalEnv.MaxY, envWgs84.MaxY);
            }
        }

        entry.properties["vector"] = {
            {"driver", GDALGetDriverShortName(GDALGetDatasetDriver(hDS))},
            {"layers", layersArr}};
        if (!cadJson.is_null()) entry.properties["vector"].update(cadJson);

        if (haveEnv)
        {
            // Populate polygon_geom as 5-point closed ring in CCW order.
            entry.polygon_geom.addPoint(totalEnv.MinX, totalEnv.MinY, 0.0);
            entry.polygon_geom.addPoint(totalEnv.MaxX, totalEnv.MinY, 0.0);
            entry.polygon_geom.addPoint(totalEnv.MaxX, totalEnv.MaxY, 0.0);
            entry.polygon_geom.addPoint(totalEnv.MinX, totalEnv.MaxY, 0.0);
            entry.polygon_geom.addPoint(totalEnv.MinX, totalEnv.MinY, 0.0);

            entry.point_geom.addPoint((totalEnv.MinX + totalEnv.MaxX) / 2.0,
                                      (totalEnv.MinY + totalEnv.MaxY) / 2.0,
                                      0.0);
        }
    }
    catch (const std::exception &e)
    {
        LOGD << "Vector parse warning: " << e.what();
        entry.properties["warning"] = std::string("vector parse failed: ") + e.what();
    }
}

class VectorEntryHandler : public EntryHandler {
  public:
    VectorEntryHandler() : EntryHandler(EntryType::Vector, "Vector") {}

    std::vector<std::string> extensions() const override {
        return {"geojson", "dxf", "dwg", "shp", "shz", "fgb", "topojson", "kml", "kmz", "gpkg"};
    }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseVectorMetadata(path, entry);
    }

    std::string subfolder() const override { return VectorBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"vector.gpkg", artifactPath(VectorBuildSubfolder, VectorGpkgFileName), ArtifactKind::File,
                 "application/geopackage+sqlite3", VectorGpkgFileName, "", false},
                // The media type of a directory artifact is its index's: the TileJSON (the tiles
                // themselves, "<z>/<x>/<y>.pbf", are application/vnd.mapbox-vector-tile)
                {"vector.mvt", MvtBuildSubfolder, ArtifactKind::Directory, "application/json", "",
                 MvtMetadataFileName, false},
                {"vector.local", artifactPath(VectorBuildSubfolder, VectorLocalFileName), ArtifactKind::File,
                 "application/json", "", "", false}};
    }

    // buildVector manages its own atomic write to baseOutputPath/vec and baseOutputPath/mvt
    bool usesTempFolder() const override { return false; }

    // vec/source.gpkg AND mvt/metadata.json both exist and are not empty: two canonical
    // sentinel files rather than a directory scan, since both artifacts are always
    // co-produced and uniquely identify a complete build. A local CAD drawing (no CRS) has no
    // MVT: vec/local.json replaces mvt/metadata.json as the second sentinel.
    bool isComplete(const fs::path& baseOutputPath, bool /*reportRecoverable*/) const override {
        return fileExistsAndNonEmpty(baseOutputPath / VectorBuildSubfolder / VectorGpkgFileName) &&
               (fileExistsAndNonEmpty(baseOutputPath / MvtBuildSubfolder / MvtMetadataFileName) ||
                fileExistsAndNonEmpty(baseOutputPath / VectorBuildSubfolder / VectorLocalFileName));
    }

    void build(const BuildContext& ctx) const override {
        // Propagate the force flag so a partial build (e.g. vec/ present but mvt/ missing) is
        // fully rebuilt instead of being silently treated as up-to-date inside buildVector.
        buildVector(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.baseOutputPath), ctx.force);
    }

    // The .dbf/.shx/.cpg of a shapefile; a .prj also gives a CAD drawing its CRS (DDB-12)
    std::vector<SidecarRule> sidecarRules() const override {
        return {{{"shp"}, {"cpg", "dbf", "shx", "prj"}},
                {{"dxf", "dwg"}, {"prj"}}};
    }
};

}  // namespace

std::unique_ptr<EntryHandler> makeVectorEntryHandler() {
    return std::make_unique<VectorEntryHandler>();
}

}  // namespace ddb
