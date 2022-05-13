/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tiler.h"

#include <mutex>
#include <memory>
#include <vector>

#include "entry.h"
#include "exceptions.h"
#include "geoproject.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"
#include "userprofile.h"

namespace ddb {

std::string Tiler::getTilePath(int z, int x, int y, bool createIfNotExists) {
    if (outputFolder.empty()){
        return "/vsimem/" + utils::generateRandomString(16) + "-" + std::to_string(z) + "-" + std::to_string(x) + "-" + std::to_string(y) + ".png";
    }else{
        // TODO: retina tiles support?
        const fs::path dir = outputFolder / std::to_string(z) / std::to_string(x);
        if (createIfNotExists && !exists(dir)) {
            io::createDirectories(dir);
        }

        fs::path p = dir / fs::path(std::to_string(y) + ".png");
        return p.string();
    }
}

Tiler::Tiler(const std::string &inputPath, const std::string &outputFolder,
             int tileSize, bool tms)
    : inputPath(inputPath),
      outputFolder(outputFolder),
      tileSize(tileSize),
      tms(tms),
      mercator(GlobalMercator(tileSize)) {
    if (!fs::exists(inputPath) && !utils::isNetworkPath(inputPath))
        throw FSException(inputPath + " does not exist");
    if (tileSize <= 0 ||
        std::ceil(std::log2(tileSize) != std::floor(std::log2(tileSize))))
        throw GDALException("Tile size must be a power of 2 greater than 0");

    if (!outputFolder.empty() && !fs::exists(outputFolder)) {
        // Try to create
        io::createDirectories(outputFolder);
    }
}

Tiler::~Tiler() {
}

std::string Tiler::tile(const TileInfo &t) { return tile(t.tz, t.tx, t.ty); }

std::vector<TileInfo> Tiler::getTilesForZoomLevel(int tz) const {
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

BoundingBox<int> Tiler::getMinMaxZ() const { return BoundingBox(tMinZ, tMaxZ); }

BoundingBox<Projected2Di> Tiler::getMinMaxCoordsForZ(int tz) const {
    BoundingBox b(mercator.metersToTile(oMinX, oMinY, tz),
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

template <typename T>
void Tiler::rescale(GDALRasterBandH hBand, char *buffer, size_t bufsize) {
    double minmax[2];
    GDALComputeRasterMinMax(hBand, TRUE, minmax);

    // Avoid divide by zero
    if (minmax[0] == minmax[1]) minmax[1] += 0.1;

    LOGD << "Min: " << minmax[0] << " | Max: " << minmax[1];

    // Can still happen according to GDAL for very large values
    if (minmax[0] == minmax[1])
        throw GDALException(
            "Cannot scale values due to source min/max being equal");

    double deltamm = minmax[1] - minmax[0];
    T *ptr = reinterpret_cast<T *>(buffer);

    for (size_t i = 0; i < bufsize; i++) {
        ptr[i] = static_cast<T>(((ptr[i] - minmax[0]) / deltamm) * 255.0);
    }
}

int Tiler::tmsToXYZ(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;
}

int Tiler::xyzToTMS(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;  // The same!
}

GlobalMercator::GlobalMercator(int tileSize) : tileSize(tileSize) {
    originShift = 2.0 * M_PI * 6378137.0 / 2.0;
    initialResolution = 2.0 * M_PI * 6378137.0 / static_cast<double>(tileSize);
    maxZoomLevel = 99;
}

BoundingBox<Geographic2D> GlobalMercator::tileLatLonBounds(int tx, int ty,
                                                           int zoom) const {
    const BoundingBox<Projected2D> bounds = tileBounds(tx, ty, zoom);
    const Geographic2D min = metersToLatLon(bounds.min.x, bounds.min.y);
    const Geographic2D max = metersToLatLon(bounds.max.x, bounds.max.y);
    return BoundingBox<Geographic2D>(min, max);
}

BoundingBox<Projected2D> GlobalMercator::tileBounds(int tx, int ty,
                                                    int zoom) const {
    const Projected2D min = pixelsToMeters(tx * tileSize, ty * tileSize, zoom);
    const Projected2D max =
        pixelsToMeters((tx + 1) * tileSize, (ty + 1) * tileSize, zoom);
    return BoundingBox<Projected2D>(min, max);
}

Geographic2D GlobalMercator::metersToLatLon(double mx, double my) const {
    double lon = mx / originShift * 180.0;
    double lat = my / originShift * 180.0;

    lat = 180.0 / M_PI *
          (2 * std::atan(std::exp(lat * M_PI / 180.0)) - M_PI / 2.0);
    return Geographic2D(lon, lat);
}

Projected2Di GlobalMercator::metersToTile(double mx, double my,
                                          int zoom) const {
    const Projected2D p = metersToPixels(mx, my, zoom);
    return pixelsToTile(p.x, p.y);
}

Projected2D GlobalMercator::pixelsToMeters(int px, int py, int zoom) const {
    const double res = resolution(zoom);
    return Projected2D(px * res - originShift, py * res - originShift);
}

Projected2D GlobalMercator::metersToPixels(double mx, double my,
                                           int zoom) const {
    const double res = resolution(zoom);
    return Projected2D((mx + originShift) / res, (my + originShift) / res);
}

Projected2Di GlobalMercator::pixelsToTile(double px, double py) const {
    return Projected2Di(
        static_cast<int>(std::ceil(px / static_cast<double>(tileSize)) - 1),
        static_cast<int>(std::ceil(py / static_cast<double>(tileSize)) - 1));
}

double GlobalMercator::resolution(int zoom) const {
    return initialResolution / std::pow(2, zoom);
}

int GlobalMercator::zoomForLength(double meterLength) const{
    return std::round(std::log(initialResolution / meterLength * tileSize) / std::log(2));
}

int GlobalMercator::zoomForPixelSize(double pixelSize) const {
    for (int i = 0; i < maxZoomLevel; i++) {
        if (pixelSize > resolution(i)) {
            return i - 1;
        }
    }
    LOGD << "Exceeded max zoom level";
    return 0;
}

}  // namespace ddb
