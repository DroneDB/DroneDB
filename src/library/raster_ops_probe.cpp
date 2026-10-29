/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The probe operation of the raster helper (raster_ops.h): what GDAL finds in a raster or a vector
// file, as the entry of the file needs it. Rasters: size, geotransform, projection, WGS84
// footprint and bands of a GeoRaster. Vectors: layers, extent and, for a CAD drawing, its
// coordinate system.

#include <algorithm>
#include <limits>

#include "cad.h"
#include "cog_utils.h"
#include "coordstransformer.h"
#include "cpl_conv.h"
#include "entry.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "gdal_priv.h"
#include "gdal_raii.h"
#include "logger.h"
#include "mio.h"
#include "ogr_api.h"
#include "ogr_spatialref.h"
#include "raster_ops.h"

namespace ddb {
namespace raster {

namespace {

RasterOpException cannotOpen(const fs::path &path)
{
    return RasterOpException(ErrorCode::Unreadable, "Cannot open " + io::toUtf8(path) + " for reading");
}

// ---- rasters

// The WGS84 footprint (polygon) and center (point) of the raster, from its projection (WKT, not
// empty) and geotransform. The footprint is left out when GDAL cannot transform the CRS.
void addFootprint(ProbeResult &result, const std::string &wkt, const double *geotransform, int width, int height,
                  const fs::path &path)
{
    LOGV << "Setting projection property";
    result.wkt = wkt;

    // Get lat/lon extent of raster
    const SrsPtr srs = srsFromWkt(wkt.c_str());
    LOGV << "OSRImportFromWkt result: " << (srs ? "Success" : "Failed");

    if (!srs)
        throw GDALException("Cannot read spatial reference system for " + io::toUtf8(path) + ". Is PROJ available?");

    const SrsPtr wgs84 = srsFromEpsg(4326);
    if (!wgs84)
        throw GDALException("Cannot read WGS84 spatial reference system for " + io::toUtf8(path) + ". Is PROJ available?");

    LOGV << "OSRImportFromEPSG result: Success";

    const auto transform = CoordsTransformer::tryCreate(srsOf(srs), srsOf(wgs84));
    LOGV << "Created coordinate transformation: " << (transform ? "Success" : "Failed");

    if (!transform)
    {
        LOGV << "Failed to create coordinate transformation";
        return;
    }

    OGRCoordinateTransformationH hTransform = OGRCoordinateTransformation::ToHandle(transform->get());
    LOGV << "Computing corner coordinates";

    auto ul = getRasterCoordinate(hTransform, geotransform, 0.0, 0.0);
    LOGV << "Upper Left: " << ul.longitude << ", " << ul.latitude;

    auto ur = getRasterCoordinate(hTransform, geotransform, width, 0);
    LOGV << "Upper Right: " << ur.longitude << ", " << ur.latitude;

    auto lr = getRasterCoordinate(hTransform, geotransform, width, height);
    LOGV << "Lower Right: " << lr.longitude << ", " << lr.latitude;

    auto ll = getRasterCoordinate(hTransform, geotransform, 0.0, height);
    LOGV << "Lower Left: " << ll.longitude << ", " << ll.latitude;

    LOGV << "Adding points to polygon geometry";
    result.polygonGeom.addPoint(ul.longitude, ul.latitude, 0.0);
    result.polygonGeom.addPoint(ur.longitude, ur.latitude, 0.0);
    result.polygonGeom.addPoint(lr.longitude, lr.latitude, 0.0);
    result.polygonGeom.addPoint(ll.longitude, ll.latitude, 0.0);
    result.polygonGeom.addPoint(ul.longitude, ul.latitude, 0.0);

    auto center = getRasterCoordinate(hTransform, geotransform, width / 2.0, height / 2.0);
    LOGV << "Center point: " << center.longitude << ", " << center.latitude;
    result.pointGeom.addPoint(center.longitude, center.latitude, 0.0);
}

// The data type and color interpretation of every band
json describeBands(GDALDatasetH hDataset)
{
    int bandCount = GDALGetRasterCount(hDataset);
    LOGV << "Number of raster bands: " << bandCount;

    json bands = json::array();
    for (int i = 0; i < bandCount; i++)
    {
        LOGV << "Processing band " << (i + 1) << " of " << bandCount;

        GDALRasterBandH hBand = GDALGetRasterBand(hDataset, i + 1);
        if (hBand != nullptr)
        {
            auto b = json::object();

            GDALDataType dataType = GDALGetRasterDataType(hBand);
            const char* dataTypeName = GDALGetDataTypeName(dataType);
            LOGV << "Band " << (i + 1) << " data type: " << dataTypeName;
            b["type"] = dataTypeName;

            GDALColorInterp colorInterp = GDALGetRasterColorInterpretation(hBand);
            const char* colorInterpName = GDALGetColorInterpretationName(colorInterp);
            LOGV << "Band " << (i + 1) << " color interpretation: " << colorInterpName;
            b["colorInterp"] = colorInterpName;

            bands.push_back(b);
        }
        else
        {
            LOGV << "Failed to get band " << (i + 1);
        }
    }
    return bands;
}

// Size, geotransform, projection, WGS84 footprint and bands
void describeGeoRaster(GDALDatasetH hDataset, const fs::path &path, ProbeResult &result)
{
    int width = GDALGetRasterXSize(hDataset);
    int height = GDALGetRasterYSize(hDataset);

    LOGV << "Raster dimensions - Width: " << width << ", Height: " << height;

    result.properties["width"] = width;
    result.properties["height"] = height;

    double geotransform[6];
    CPLErr geoTransformResult = GDALGetGeoTransform(hDataset, geotransform);
    LOGV << "GDALGetGeoTransform result: " << (geoTransformResult == CE_None ? "Success" : "Failed");

    if (geoTransformResult == CE_None)
    {
        LOGV << "Geotransform values: ["
             << geotransform[0] << ", " << geotransform[1] << ", " << geotransform[2] << ", "
             << geotransform[3] << ", " << geotransform[4] << ", " << geotransform[5] << "]";

        result.properties["geotransform"] = json::array();
        for (int i = 0; i < 6; i++)
            result.properties["geotransform"].push_back(geotransform[i]);

        const char *projectionRef = GDALGetProjectionRef(hDataset);
        if (projectionRef != nullptr && projectionRef[0] != '\0')
            addFootprint(result, projectionRef, geotransform, width, height, path);
        else
            LOGD << "Projection is empty";
    }
    else
    {
        LOGV << "No geotransform found in dataset";
    }

    result.properties["bands"] = describeBands(hDataset);
}

ProbeResult probeRaster(const fs::path &path)
{
    const std::string utf8 = io::toUtf8(path);
    ProbeResult result;
    {
        // Owned: the SRS checks throw while the dataset is open
        const auto dataset = openUserRaster(utf8);
        if (!dataset)
            throw cannotOpen(path);

        GDALDatasetH hDataset = datasetHandle(dataset);
        const char *projectionRef = GDALGetProjectionRef(hDataset);
        if (projectionRef == nullptr || projectionRef[0] == '\0')
        {
            result.type = "Raster";
            return result;
        }

        result.type = "GeoRaster";
        describeGeoRaster(hDataset, path, result);
    }
    result.isCog = isOptimizedCog(utf8);
    return result;
}

// ---- vectors

// The name, geometry type and feature count of a layer (the layer index names an unnamed one)
json layerSummary(OGRLayerH hLayer, int i)
{
    auto layerJson = json::object();
    const char *layerName = OGR_L_GetName(hLayer);
    layerJson["name"] = layerName ? layerName : ("layer_" + std::to_string(i));
    layerJson["geometryType"] = OGRGeometryTypeToName(OGR_L_GetGeomType(hLayer));
    // A driver without a fast count (GPX) answers -1: count the features then
    GIntBig count = OGR_L_GetFeatureCount(hLayer, FALSE);
    if (count < 0)
        count = OGR_L_GetFeatureCount(hLayer, TRUE);
    layerJson["featureCount"] = static_cast<int64_t>(count);
    return layerJson;
}

// A local CAD drawing (no CRS) has no footprint: nothing to place on a map. Its layer extents
// stay in drawing coordinates.
json describeLocalCad(GDALDatasetH hDS, const cad::CadCrs &crs, const json &cadJson)
{
    auto layers = json::array();
    for (int i = 0; i < GDALDatasetGetLayerCount(hDS); i++)
    {
        OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
        if (hLayer == nullptr) continue;
        layers.push_back(layerSummary(hLayer, i));
    }
    json vector = {
        {"driver", GDALGetDriverShortName(GDALGetDatasetDriver(hDS))},
        {"layers", layers}};
    vector.update(cadJson);
    if (crs.hasExtent)
        vector["localExtent"] = {crs.extent.MinX, crs.extent.MinY, crs.extent.MaxX, crs.extent.MaxY};
    return vector;
}

// The extent of a layer in WGS84. The layer's own SRS (or the one of its CAD drawing) decides
// whether the corners need a reprojection; wgs84 is made on first use.
OGREnvelope layerEnvelopeWgs84(OGRLayerH hLayer, int i, const OGREnvelope &env, OGRSpatialReferenceH fallbackSrs,
                               SrsPtr &wgs84)
{
    OGRSpatialReferenceH hSrs = OGR_L_GetSpatialRef(hLayer);
    if (hSrs == nullptr) hSrs = fallbackSrs; // CAD layers carry no SRS
    OGREnvelope envWgs84 = env;

    if (hSrs == nullptr)
    {
        LOGD << "Vector parse: layer " << i << " has no SRS, assuming WGS84";
        return envWgs84;
    }

    // Check if reprojection is needed (skip if already WGS84).
    const char *authCode = OSRGetAuthorityCode(hSrs, nullptr);
    const char *authName = OSRGetAuthorityName(hSrs, nullptr);
    bool isWgs84 = (authName && authCode &&
                    std::string(authName) == "EPSG" &&
                    std::string(authCode) == "4326");
    if (isWgs84) return envWgs84;

    if (!wgs84)
    {
        wgs84 = srsFromEpsg(4326);
        if (!wgs84)
        {
            LOGD << "Vector parse: failed to import EPSG:4326";
        }
    }
    if (!wgs84) return envWgs84;

    const auto toWgs84 =
        CoordsTransformer::tryCreate(*OGRSpatialReference::FromHandle(hSrs), srsOf(wgs84));
    if (!toWgs84) return envWgs84;

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
    return envWgs84;
}

// The layers of the dataset, and the envelope of them all in WGS84 (when any has an extent)
struct VectorLayers
{
    json layers = json::array();
    OGREnvelope totalEnv;
    bool haveEnv = false;
};

VectorLayers describeLayers(GDALDatasetH hDS, OGRSpatialReferenceH fallbackSrs)
{
    VectorLayers out;
    // Lazy WGS84 SRS - only allocate when needed (first layer with SRS).
    SrsPtr wgs84;
    // Convex envelope in WGS84 across all layers.
    out.totalEnv.MinX = out.totalEnv.MinY =  std::numeric_limits<double>::max();
    out.totalEnv.MaxX = out.totalEnv.MaxY = -std::numeric_limits<double>::max();

    const int layerCount = GDALDatasetGetLayerCount(hDS);
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

        const OGREnvelope envWgs84 = layerEnvelopeWgs84(hLayer, i, env, fallbackSrs, wgs84);

        // Per-layer metadata
        auto layerJson = layerSummary(hLayer, i);
        layerJson["extent"] = {envWgs84.MinX, envWgs84.MinY, envWgs84.MaxX, envWgs84.MaxY};
        out.layers.push_back(layerJson);

        // Accumulate global envelope
        if (!out.haveEnv)
        {
            out.totalEnv = envWgs84;
            out.haveEnv = true;
        }
        else
        {
            out.totalEnv.MinX = std::min(out.totalEnv.MinX, envWgs84.MinX);
            out.totalEnv.MinY = std::min(out.totalEnv.MinY, envWgs84.MinY);
            out.totalEnv.MaxX = std::max(out.totalEnv.MaxX, envWgs84.MaxX);
            out.totalEnv.MaxY = std::max(out.totalEnv.MaxY, envWgs84.MaxY);
        }
    }
    return out;
}

ProbeResult probeVector(const fs::path &path, const ProbeParams &params)
{
    // The drawing the user named; `path` is what GDAL reads (a DXF made from a DWG)
    const fs::path &source = params.source.empty() ? path : params.source;

    ProbeResult result;
    result.type = "Vector";

    const auto dataset = openUserVector(io::toUtf8(path));
    if (!dataset)
        throw cannotOpen(path);
    GDALDatasetH hDS = datasetHandle(dataset);
    SrsPtr cadSrs;

    json cadJson;
    if (cad::isCadFile(source))
    {
        const auto info = cad::readDxfInfo(path);
        const auto crs = cad::resolveCadCrs(source, hDS, info);
        cadJson = {{"crs", {{"source", crs.source}, {"label", crs.label}}},
                   {"units", cad::insUnitsName(info.insUnits)}};
        if (params.converted) cadJson["converter"] = "dwg2dxf";
        result.properties["georeferenced"] = crs.georeferenced;

        if (!crs.georeferenced)
        {
            result.properties["vector"] = describeLocalCad(hDS, crs, cadJson);
            return result;
        }

        if (!crs.srs.empty())
        {
            cadSrs = srsFromUserInput(crs.srs.c_str());
            if (!cadSrs)
                throw GDALException("Invalid CAD coordinate system " + crs.label);
        }
    }

    const VectorLayers described = describeLayers(hDS, cadSrs.get());
    const OGREnvelope &totalEnv = described.totalEnv;

    result.properties["vector"] = {
        {"driver", GDALGetDriverShortName(GDALGetDatasetDriver(hDS))},
        {"layers", described.layers}};
    if (!cadJson.is_null()) result.properties["vector"].update(cadJson);

    if (described.haveEnv)
    {
        // A closed ring of five points, counter-clockwise
        result.polygonGeom.addPoint(totalEnv.MinX, totalEnv.MinY, 0.0);
        result.polygonGeom.addPoint(totalEnv.MaxX, totalEnv.MinY, 0.0);
        result.polygonGeom.addPoint(totalEnv.MaxX, totalEnv.MaxY, 0.0);
        result.polygonGeom.addPoint(totalEnv.MinX, totalEnv.MaxY, 0.0);
        result.polygonGeom.addPoint(totalEnv.MinX, totalEnv.MinY, 0.0);

        result.pointGeom.addPoint((totalEnv.MinX + totalEnv.MaxX) / 2.0,
                                  (totalEnv.MinY + totalEnv.MaxY) / 2.0,
                                  0.0);
    }
    return result;
}

}  // namespace

ProbeResult probe(const fs::path &path, const ProbeParams &params)
{
    return params.kind == ProbeKind::Vector ? probeVector(path, params) : probeRaster(path);
}

void applyProbe(const ProbeResult &result, Entry &entry)
{
    for (const auto &[key, value] : result.properties.items())
        entry.properties[key] = value;
    if (!result.wkt.empty())
        entry.properties["projection"] = result.wkt;
    for (const auto &p : result.pointGeom.points)
        entry.point_geom.addPoint(p.x, p.y, p.z);
    for (const auto &p : result.polygonGeom.points)
        entry.polygon_geom.addPoint(p.x, p.y, p.z);
}

}  // namespace raster
}  // namespace ddb
