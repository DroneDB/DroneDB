/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// GeoRaster entries: georeferenced rasters, built into a Cloud Optimized GeoTIFF. The
// Image handler detects them (a TIFF with a projection), entry_handler_image.cpp.

#include <memory>

#include "build_layout.h"
#include "json.h"
#include "cog.h"
#include "cpl_conv.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gdal_priv.h"
#include "coordstransformer.h"
#include "gdal_raii.h"
#include "ogr_spatialref.h"

namespace ddb {

namespace {

// The WGS84 footprint (polygon) and center (point) of the raster, from its projection (WKT, not
// empty) and geotransform. The footprint is left out when GDAL cannot transform the CRS.
void addFootprint(Entry &entry, const std::string &wkt, const double *geotransform, int width, int height,
                  const fs::path &path)
{
    LOGV << "Setting projection property";
    // Set projection
    entry.properties["projection"] = wkt;

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
    entry.polygon_geom.addPoint(ul.longitude, ul.latitude, 0.0);
    entry.polygon_geom.addPoint(ur.longitude, ur.latitude, 0.0);
    entry.polygon_geom.addPoint(lr.longitude, lr.latitude, 0.0);
    entry.polygon_geom.addPoint(ll.longitude, ll.latitude, 0.0);
    entry.polygon_geom.addPoint(ul.longitude, ul.latitude, 0.0);

    auto center = getRasterCoordinate(hTransform, geotransform, width / 2.0, height / 2.0);
    LOGV << "Center point: " << center.longitude << ", " << center.latitude;
    entry.point_geom.addPoint(center.longitude, center.latitude, 0.0);
}

// The data type and color interpretation of every band
void describeBands(GDALDatasetH hDataset, Entry &entry)
{
    int bandCount = GDALGetRasterCount(hDataset);
    LOGV << "Number of raster bands: " << bandCount;

    entry.properties["bands"] = json::array();
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

            entry.properties["bands"].push_back(b);
        }
        else
        {
            LOGV << "Failed to get band " << (i + 1);
        }
    }
}

// Size, geotransform, projection, WGS84 footprint and bands
void parseGeoRasterMetadata(const fs::path &path, Entry &entry)
{
    LOGV << "Processing GeoRaster file: " << io::toUtf8(path);

    // Owned: the SRS checks below throw while the dataset is open
    auto dataset = adoptDataset(GDALOpen(io::toUtf8(path).c_str(), GA_ReadOnly));
    if (!dataset)
    {
        LOGV << "GDAL failed to open dataset: " << io::toUtf8(path);
        throw GDALException("Cannot open " + io::toUtf8(path) + " for reading");
    }

    LOGV << "GDAL successfully opened dataset";
    GDALDatasetH hDataset = datasetHandle(dataset);

    int width = GDALGetRasterXSize(hDataset);
    int height = GDALGetRasterYSize(hDataset);

    LOGV << "Raster dimensions - Width: " << width << ", Height: " << height;

    entry.properties["width"] = width;
    entry.properties["height"] = height;

    double geotransform[6];
    CPLErr geoTransformResult = GDALGetGeoTransform(hDataset, geotransform);
    LOGV << "GDALGetGeoTransform result: " << (geoTransformResult == CE_None ? "Success" : "Failed");

    if (geoTransformResult == CE_None)
    {
        LOGV << "Geotransform values: ["
             << geotransform[0] << ", " << geotransform[1] << ", " << geotransform[2] << ", "
             << geotransform[3] << ", " << geotransform[4] << ", " << geotransform[5] << "]";

        entry.properties["geotransform"] = json::array();
        for (int i = 0; i < 6; i++)
            entry.properties["geotransform"].push_back(geotransform[i]);

        const char* projectionRef = GDALGetProjectionRef(hDataset);
        LOGV << "GDALGetProjectionRef result: " << (projectionRef != nullptr ? "Found" : "NULL");

        if (projectionRef != nullptr)
        {
            std::string wkt = projectionRef;
            LOGV << "WKT string length: " << wkt.length();
            LOGV << "WKT content: " << wkt;

            if (!wkt.empty())
                addFootprint(entry, wkt, geotransform, width, height, path);
            else
                LOGD << "Projection is empty";
        }
        else
        {
            LOGV << "No projection reference found in dataset";
        }
    }
    else
    {
        LOGV << "No geotransform found in dataset";
    }

    describeBands(hDataset, entry);

    LOGV << "Closing GDAL dataset";
    dataset.reset();
    LOGV << "GeoRaster processing completed successfully";
}

class GeoRasterEntryHandler : public EntryHandler {
  public:
    GeoRasterEntryHandler() : EntryHandler(EntryType::GeoRaster, "GeoRaster") {}

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parseGeoRasterMetadata(path, entry);
    }

    std::string subfolder() const override { return CogBuildSubfolder; }

    std::vector<BuildArtifact> artifacts() const override {
        return {{"raster.cog", artifactPath(CogBuildSubfolder, CogFileName), ArtifactKind::File,
                 "image/tiff; application=geotiff; profile=cloud-optimized", CogFileName, "", false}};
    }

    void build(const BuildContext& ctx) const override {
        buildCog(io::toUtf8(ctx.inputPath), io::toUtf8(ctx.tempFolder / CogFileName));
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::File; }

    bool tilesWithoutReprojection() const override { return true; }
};

}  // namespace

std::unique_ptr<EntryHandler> makeGeoRasterEntryHandler() {
    return std::make_unique<GeoRasterEntryHandler>();
}

}  // namespace ddb
