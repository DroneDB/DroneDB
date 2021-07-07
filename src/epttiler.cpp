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
#include <pdal/filters/ColorinterpFilter.hpp>

#include "entry.h"
#include "exceptions.h"
#include "coordstransformer.h"
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
      wSize(tileSize * tileSize),
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

    bool hasColors = std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Red") != eptInfo.dimensions.end() &&
                std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Green") != eptInfo.dimensions.end() &&
                std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Blue") != eptInfo.dimensions.end();
    LOGD << "Has colors: " << (hasColors ? "true" : "false");

    // Get bounds of tile (3857), convert to EPT CRS
    auto tileBounds = mercator.tileBounds(tx, ty, tz);
    auto bounds = tileBounds;

    // Expand by a few meters, so that we have sufficient
    // overlap with other tiles
    const int boundsBufSize = 5; // meters
    bounds.min.x -= boundsBufSize;
    bounds.max.x += boundsBufSize;
    bounds.min.y -= boundsBufSize;
    bounds.max.y += boundsBufSize;

    CoordsTransformer ct(3857, eptInfo.wktProjection);
    ct.transform(&bounds.min.x, &bounds.min.y);
    ct.transform(&bounds.max.x, &bounds.max.y);

    pdal::Options eptOpts;
    eptOpts.add("filename", eptPath);
    std::stringstream ss;
    ss << std::setprecision(14) << "([" << bounds.min.x << "," << bounds.min.y << "], " <<
                                    "[" << bounds.max.x << "," << bounds.max.y << "])";
    eptOpts.add("bounds", ss.str());
    eptOpts.add("resolution", mercator.resolution(tz - 1)); // TODO! change

    pdal::EptReader eptReader;
    pdal::Stage *main = &eptReader;
    eptReader.setOptions(eptOpts);

    pdal::ColorinterpFilter colorFilter;
    if (!hasColors || true){
        // Add ramp filter
        LOGD << "Adding ramp filter (" << eptInfo.bounds[2] << ", " << eptInfo.bounds[5] << ")";

        pdal::Options cfOpts;
        cfOpts.add("ramp", "pestel_shades");
        cfOpts.add("minimum", eptInfo.bounds[2]);
        cfOpts.add("maximum", eptInfo.bounds[5]);
        colorFilter.setOptions(cfOpts);
        colorFilter.setInput(eptReader);
        main = &colorFilter;
    }

    pdal::PointTable table;
    main->prepare(table);
    pdal::PointViewSet point_view_set = main->execute(table);
    pdal::PointViewPtr point_view = *point_view_set.begin();
    pdal::Dimension::IdList dims = point_view->dims();

    const int nBands = 3;
    const int bufSize = GDALGetDataTypeSizeBytes(GDT_Byte) * wSize;
    std::unique_ptr<uint8_t> buffer(new uint8_t[bufSize * nBands]);
    std::unique_ptr<uint8_t> alphaBuffer(new uint8_t[bufSize]);
    std::unique_ptr<float> zBuffer(new float[bufSize]);

    memset(buffer.get(), 0, bufSize * nBands);
    memset(alphaBuffer.get(), 255, bufSize);
    memset(zBuffer.get(), std::numeric_limits<float>::min(), bufSize);

    LOGD << "Fetched " << point_view->size() << " points";

    const double tileScaleW = tileSize / (tileBounds.max.x - tileBounds.min.x);
    const double tileScaleH = tileSize / (tileBounds.max.y - tileBounds.min.y);
    CoordsTransformer ict(eptInfo.wktProjection, 3857);

    for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
        auto p = point_view->point(idx);
        double x = p.getFieldAs<double>(pdal::Dimension::Id::X);
        double y = p.getFieldAs<double>(pdal::Dimension::Id::Y);
        double z = p.getFieldAs<double>(pdal::Dimension::Id::Z);

        ict.transform(&x, &y);

        // Map projected coordinates to local PNG coordinates
        int px = std::round((x - tileBounds.min.x) * tileScaleW);
        int py = tileSize - 1 - std::round((y - tileBounds.min.y) * tileScaleH);

        if (px >= 0 && px < tileSize && py >= 0 && py < tileSize){
            // Within bounds
            uint8_t red = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Red);
            uint8_t green = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Green);
            uint8_t blue = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Blue);

            if (zBuffer.get()[py * tileSize + px] < z){
                zBuffer.get()[py * tileSize + px] = z;
                drawCircle(buffer.get(), px, py, 2, red, green, blue);
            }
        }
    }

    GDALDriverH memDrv = GDALGetDriverByName("MEM");
    if (memDrv == nullptr) throw GDALException("Cannot create MEM driver");
    GDALDriverH pngDrv = GDALGetDriverByName("PNG");
    if (pngDrv == nullptr) throw GDALException("Cannot create PNG driver");

    // Need to create in-memory dataset
    // (PNG driver does not have Create() method)
    const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, nBands + 1,
                                           GDT_Byte, nullptr);
    if (dsTile == nullptr) throw GDALException("Cannot create dsTile");

    if (GDALDatasetRasterIO(dsTile, GF_Write, 0, 0, tileSize,
                            tileSize, buffer.get(), tileSize, tileSize,
                            GDT_Byte, nBands, nullptr, 0, 0,
                            0) != CE_None) {
        throw GDALException("Cannot write tile data");
    }

    const GDALRasterBandH tileAlphaBand =
        GDALGetRasterBand(dsTile, nBands + 1);
    GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

    if (GDALRasterIO(tileAlphaBand, GF_Write, 0, 0, tileSize,
                     tileSize, alphaBuffer.get(), tileSize, tileSize,
                     GDT_Byte, 0, 0) != CE_None) {
        throw GDALException("Cannot write tile alpha data");
    }


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

int EptTiler::tmsToXYZ(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;
}

int EptTiler::xyzToTMS(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;  // The same!
}

void EptTiler::drawCircle(uint8_t *buffer, int px, int py, int radius, uint8_t r, uint8_t g, uint8_t b){
    int r2 = radius * radius;
    int area = r2 << 2;
    int rr = radius << 1;

    for (int i = 0; i < area; i++){
        int tx = (i % rr) - radius;
        int ty = (i / rr) - radius;
        if (tx * tx + ty * ty <= r2){
            int dx = px + tx;
            int dy = py + ty;
            if (dx >= 0 && dx < tileSize && dy >= 0 && dy < tileSize){
                buffer[dy * tileSize + dx + wSize * 0] = r;
                buffer[dy * tileSize + dx + wSize * 1] = g;
                buffer[dy * tileSize + dx + wSize * 2] = b;
            }
        }
    }
}


}  // namespace ddb
