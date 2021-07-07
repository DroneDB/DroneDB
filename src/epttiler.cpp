/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "epttiler.h"

#include <mutex>
#include <memory>
#include <vector>

#include <pdal/Options.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/io/EptReader.hpp>
#include <pdal/filters/ReprojectionFilter.hpp>

#include "entry.h"
#include "exceptions.h"
#include "geoproject.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"
#include "userprofile.h"

namespace ddb {


std::string EptTiler::getTilePath(int z, int x, int y, bool createIfNotExists) {
    // TODO: retina tiles support?
    const fs::path dir = outputFolder / std::to_string(z) / std::to_string(x);
    if (createIfNotExists && !fs::exists(dir)) {
        io::createDirectories(dir);
    }

    fs::path p = dir / fs::path(std::to_string(y) + ".png");
    return p.string();
}

EptTiler::EptTiler(const std::string &eptPath, const std::string &outputFolder,
             int tileSize, bool tms)
    : eptPath(eptPath),
      outputFolder(outputFolder),
      tileSize(tileSize),
      tms(tms),
      mercator(GlobalMercator(tileSize)) {
    if (!fs::exists(eptPath))
        throw FSException(eptPath + " does not exists");
    if (tileSize <= 0 ||
        std::ceil(std::log2(tileSize) != std::floor(std::log2(tileSize))))
        throw GDALException("Tile size must be a power of 2 greater than 0");

    if (!fs::exists(outputFolder)) {
        // Try to create
        io::createDirectories(outputFolder);
    }

    // Open EPT
    if (!getEptInfo(eptPath, eptInfo, 3857)){
        throw InvalidArgsException("Cannot get EPT info for " + eptPath);
    }

    if (eptInfo.wktProjection.empty()){
        throw InvalidArgsException("EPT file has no WKT SRS: " + eptPath);
    }

    oMinX = eptInfo.polyBounds.getPoint(0).y;
    oMaxX = eptInfo.polyBounds.getPoint(2).y;
    oMaxY = eptInfo.polyBounds.getPoint(2).x;
    oMinY = eptInfo.polyBounds.getPoint(0).x;

    LOGD << "Bounds (output SRS): " << oMinX << "," << oMinY << "," << oMaxX
         << "," << oMaxY;

    // Max/min zoom level
    tMaxZ = mercator.zoomForPixelSize(0.05); // TODO: better heuristic?
    tMinZ = mercator.zoomForPixelSize(1); // TODO: better heuristic?

    LOGD << "MinZ: " << tMinZ;
    LOGD << "MaxZ: " << tMaxZ;

}

EptTiler::~EptTiler() {
}

std::string EptTiler::tile(int tz, int tx, int ty) {
    std::string tilePath = getTilePath(tz, tx, ty, true);

    if (tms) {
        ty = tmsToXYZ(ty, tz);
        LOGD << "TY: " << ty;
    }

    BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
    if (!tMinMax.contains(tx, ty)) throw GDALException("Out of bounds");

    pdal::Options eptOpts;
    eptOpts.add("filename", eptPath);

    // Get bounds of tile (3857), convert to EPT CRS
    auto tileBounds = mercator.tileBounds(tx, ty, tz);
    auto bounds = tileBounds;

    OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
    OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);

    char *wkt = strdup(eptInfo.wktProjection.c_str());
    if (OSRImportFromWkt(hTgt, &wkt) != OGRERR_NONE){
        throw GDALException("Cannot import spatial reference system " + eptInfo.wktProjection + ". Is PROJ available?");
    }
    OSRImportFromEPSG(hSrs, 3857);
    OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hTgt);

    double none = 0.0;
    if (!OCTTransform(hTransform, 1, &bounds.min.x, &bounds.min.y, &none) ||
        !OCTTransform(hTransform, 1, &bounds.max.x, &bounds.max.y, &none)){
        throw GDALException("Reprojection of bounds failed");
    }

    OCTDestroyCoordinateTransformation(hTransform);
    OSRDestroySpatialReference(hTgt);
    OSRDestroySpatialReference(hSrs);

    std::stringstream ss;
    ss << std::setprecision(14) << "([" << bounds.min.x << "," << bounds.min.y <<
                   "], [" << bounds.max.x << "," << bounds.max.y << "])";
    eptOpts.add("bounds", ss.str());
    eptOpts.add("resolution", mercator.resolution(tz - 2)); // TODO! change

    pdal::EptReader eptReader;
    eptReader.setOptions(eptOpts);

    pdal::Options reOpts;
    reOpts.add("out_srs", "EPSG:3857");
    pdal::ReprojectionFilter reFilter;
    reFilter.setOptions(reOpts);
    reFilter.setInput(eptReader);

    pdal::PointTable table;
    reFilter.prepare(table);
    pdal::PointViewSet point_view_set = reFilter.execute(table);
    pdal::PointViewPtr point_view = *point_view_set.begin();
    pdal::Dimension::IdList dims = point_view->dims();

    // TODO: what if RGB colors are missing? elevation min/max
    int nBands = 3;
    int wSize = tileSize * tileSize;
    int bufSize = GDALGetDataTypeSizeBytes(GDT_Byte) * wSize;
    uint8_t *buffer = new uint8_t[bufSize * nBands];
    memset(buffer, 0, bufSize * nBands);
    uint8_t *alphaBuffer = new uint8_t[bufSize];
    memset(alphaBuffer, 255, bufSize);

    bool hasColors = false;
    LOGD << "Fetched " << point_view->size() << " points";

    if (point_view->size() > 0){
        auto p = point_view->point(0);
        hasColors = p.hasDim(pdal::Dimension::Id::Red) &&
                    p.hasDim(pdal::Dimension::Id::Green) &&
                    p.hasDim(pdal::Dimension::Id::Blue);
    }

    double tileScaleW = (tileBounds.max.x - tileBounds.min.x) / 255.0;
    double tileScaleH = (tileBounds.max.y - tileBounds.min.y) / 255.0;

    for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
        auto p = point_view->point(idx);
        double x = p.getFieldAs<double>(pdal::Dimension::Id::X);
        double y = p.getFieldAs<double>(pdal::Dimension::Id::Y);
        double z = p.getFieldAs<double>(pdal::Dimension::Id::Z);
//        std::cout << std::setprecision(14) << x << " " << y << " " << z << std::endl;

        // Map projected coordinates to local PNG coordinates
        int px = std::round((x - tileBounds.min.x) * tileScaleW);
        int py = std::round((y - tileBounds.min.y) * tileScaleH);

        std::cout << px << " " << py << std::endl;

        if (px >= 0 && px <= 255 && py >= 0 && py <= 255){
            // Within bounds
            if (hasColors){
                uint8_t red = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Red);
                uint8_t green = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Green);
                uint8_t blue = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Blue);

                buffer[py * tileSize + px + wSize * 0] = red;
                buffer[py * tileSize + px + wSize * 1] = green;
                buffer[py * tileSize + px + wSize * 2] = blue;

                // TODO: zbuffer
            }else{
                // TODO
                throw AppException("No colors");
            }
        }
    }

//    for (int i = 0; i < tileSize; i++){
//        for (int j = 0; j < tileSize; j++){
//            buffer[i * tileSize + j + wSize * 0] = 255;
//            buffer[i * tileSize + j + wSize * 1] = 0;
//            buffer[i * tileSize + j + wSize * 2] = 0;
//        }
//    }

    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (memDrv == nullptr) throw GDALException("Cannot create MEM driver");
    GDALDriverH pngDrv = GDALGetDriverByName("PNG");
    if (pngDrv == nullptr) throw GDALException("Cannot create PNG driver");

    // Need to create in-memory dataset
    // (PNG driver does not have Create() method)
    const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, nBands + 1,
                                           GDT_Byte, nullptr);
    if (dsTile == nullptr) throw GDALException("Cannot create dsTile");

    int x = 0;
    int y = 0;
    int xsize = tileSize;
    int ysize = tileSize;

    if (GDALDatasetRasterIO(dsTile, GF_Write, x, y, xsize,
                            ysize, buffer, xsize, ysize,
                            GDT_Byte, nBands, nullptr, 0, 0,
                            0) != CE_None) {
        throw GDALException("Cannot write tile data");
    }

    const GDALRasterBandH tileAlphaBand =
        GDALGetRasterBand(dsTile, nBands + 1);
    GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

    if (GDALRasterIO(tileAlphaBand, GF_Write, x, y, xsize,
                     ysize, alphaBuffer, xsize, ysize,
                     GDT_Byte, 0, 0) != CE_None) {
        throw GDALException("Cannot write tile alpha data");
    }


    std::cout << point_view->size() << std::endl;

    const GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                              nullptr, nullptr, nullptr);
    if (outDs == nullptr)
        throw GDALException("Cannot create output dataset " + tilePath);

    GDALClose(dsTile);
    GDALClose(outDs);

    return tilePath;
}

std::string EptTiler::tile(const TileInfo &t) { return tile(t.tz, t.tx, t.ty); }

std::vector<TileInfo> EptTiler::getTilesForZoomLevel(int tz) const {
    std::vector<TileInfo> result;
    const BoundingBox<Projected2Di> bounds = getMinMaxCoordsForZ(tz);

    for (int ty = bounds.min.y; ty < bounds.max.y + 1; ty++) {
        for (int tx = bounds.min.x; tx < bounds.max.x + 1; tx++) {
            LOGD << tx << " " << ty << " " << tz;
            result.emplace_back(tx, tms ? xyzToTMS(ty, tz) : ty, tz);
        }
    }

    return result;
}

BoundingBox<int> EptTiler::getMinMaxZ() const { return BoundingBox(tMinZ, tMaxZ); }

BoundingBox<Projected2Di> EptTiler::getMinMaxCoordsForZ(int tz) const {
    BoundingBox<Projected2Di> b(mercator.metersToTile(oMinX, oMinY, tz),
                                mercator.metersToTile(oMaxX, oMaxY, tz));

    LOGD << "MinMaxCoordsForZ(" << tz << ") = (" << b.min.x << ", " << b.min.y << "), (" << b.max.x << ", " << b.max.y << ")";

    // Crop tiles extending world limits (+-180,+-90)
    b.min.x = std::max<int>(0, b.min.x);
    b.max.x = std::min<int>(static_cast<int>(std::pow(2, tz) - 1), b.max.x);

    // TODO: figure this out (TMS vs. XYZ)
    //    b.min.y = std::max<double>(0, b.min.y);
    //    b.max.y = std::min<double>(std::pow(2, tz - 1), b.max.y);

    return b;
}

BoundingBox<int> TilerHelper::parseZRange(const std::string &zRange) {
    BoundingBox<int> r;

    const std::size_t dashPos = zRange.find('-');
    if (dashPos != std::string::npos) {
        r.min = std::stoi(zRange.substr(0, dashPos));
        r.max = std::stoi(zRange.substr(dashPos + 1, zRange.length() - 1));
        if (r.min > r.max) {
            std::swap(r.min, r.max);
        }
    } else {
        r.min = r.max = std::stoi(zRange);
    }

    return r;
}


GDALDatasetH EptTiler::createWarpedVRT(const GDALDatasetH &src,
                                    const OGRSpatialReferenceH &srs,
                                    GDALResampleAlg resampling) {
    // GDALDriverH vrtDrv = GDALGetDriverByName( "VRT" );
    // if (vrtDrv == nullptr) throw GDALException("Cannot create VRT driver");

    // std::string vrtFilename = "/vsimem/" + uuidv4() + ".vrt";
    // GDALDatasetH vrt = GDALCreateCopy(vrtDrv, vrtFilename.c_str(), src,
    // FALSE, nullptr, nullptr, nullptr);

    char *dstWkt;
    if (OSRExportToWkt(srs, &dstWkt) != OGRERR_NONE)
        throw GDALException("Cannot export dst WKT " + eptPath +
                            ". Is PROJ available?");
    const char *srcWkt = GDALGetProjectionRef(src);

    const GDALDatasetH warpedVrt = GDALAutoCreateWarpedVRT(
        src, srcWkt, dstWkt, resampling, 0.001, nullptr);
    if (warpedVrt == nullptr) throw GDALException("Cannot create warped VRT");

    return warpedVrt;
}

GQResult EptTiler::geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                         double lry, int querySize) {
    GQResult o;
    double geo[6];
    if (GDALGetGeoTransform(ds, geo) != CE_None)
        throw GDALException("Cannot fetch geotransform geo");

    o.r.x = static_cast<int>((ulx - geo[0]) / geo[1] + 0.001);
    o.r.y = static_cast<int>((uly - geo[3]) / geo[5] + 0.001);
    o.r.xsize = static_cast<int>((lrx - ulx) / geo[1] + 0.5);
    o.r.ysize = static_cast<int>((lry - uly) / geo[5] + 0.5);

    if (querySize == 0) {
        o.w.xsize = o.r.xsize;
        o.w.ysize = o.r.ysize;
    } else {
        o.w.xsize = querySize;
        o.w.ysize = querySize;
    }

    o.w.x = 0;
    if (o.r.x < 0) {
        const int rxShift = std::abs(o.r.x);
        o.w.x = static_cast<int>(o.w.xsize * (static_cast<double>(rxShift) /
                                              static_cast<double>(o.r.xsize)));
        o.w.xsize = o.w.xsize - o.w.x;
        o.r.xsize =
            o.r.xsize -
            static_cast<int>(o.r.xsize * (static_cast<double>(rxShift) /
                                          static_cast<double>(o.r.xsize)));
        o.r.x = 0;
    }

    const int rasterXSize = GDALGetRasterXSize(ds);
    const int rasterYSize = GDALGetRasterYSize(ds);

    if (o.r.x + o.r.xsize > rasterXSize) {
        o.w.xsize = static_cast<int>(
            o.w.xsize *
            (static_cast<double>(rasterXSize) - static_cast<double>(o.r.x)) /
            static_cast<double>(o.r.xsize));
        o.r.xsize = rasterXSize - o.r.x;
    }

    o.w.y = 0;
    if (o.r.y < 0) {
        const int ryShift = std::abs(o.r.y);
        o.w.y = static_cast<int>(o.w.ysize * (static_cast<double>(ryShift) /
                                              static_cast<double>(o.r.ysize)));
        o.w.ysize = o.w.ysize - o.w.y;
        o.r.ysize =
            o.r.ysize -
            static_cast<int>(o.r.ysize * (static_cast<double>(ryShift) /
                                          static_cast<double>(o.r.ysize)));
        o.r.y = 0;
    }

    if (o.r.y + o.r.ysize > rasterYSize) {
        o.w.ysize = static_cast<int>(
            o.w.ysize *
            (static_cast<double>(rasterYSize) - static_cast<double>(o.r.y)) /
            static_cast<double>(o.r.ysize));
        o.r.ysize = rasterYSize - o.r.y;
    }

    return o;
}

int EptTiler::tmsToXYZ(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;
}

int EptTiler::xyzToTMS(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;  // The same!
}


}  // namespace ddb
