/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stockpile.h"
#include "exceptions.h"
#include "json.h"
#include "logger.h"
#include "utils.h"

#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <stack>
#include <string>
#include <vector>

namespace ddb {

namespace {

struct SrsHandle {
    OGRSpatialReferenceH h = nullptr;
    explicit SrsHandle(OGRSpatialReferenceH handle = nullptr) : h(handle) {}
    ~SrsHandle() { if (h) OSRDestroySpatialReference(h); }
    SrsHandle(const SrsHandle &) = delete;
    SrsHandle &operator=(const SrsHandle &) = delete;
};

struct CtHandle {
    OGRCoordinateTransformationH h = nullptr;
    ~CtHandle() { if (h) OCTDestroyCoordinateTransformation(h); }
};

struct DsGuard {
    GDALDatasetH h = nullptr;
    ~DsGuard() { if (h) GDALClose(h); }
};

// Separable 1-D gaussian blur, applied in place on a float grid.
void gaussianFilter(std::vector<float> &data, int width, int height, float sigma) {
    if (sigma < 0.1f) return;
    const int half = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    const int ksize = half * 2 + 1;
    std::vector<float> k(ksize);
    float s = 0.0f;
    for (int i = 0; i < ksize; i++) {
        const float x = static_cast<float>(i - half);
        k[i] = std::exp(-(x * x) / (2.0f * sigma * sigma));
        s += k[i];
    }
    for (auto &v : k) v /= s;

    std::vector<float> tmp(data.size(), 0.0f);
    // Horizontal pass
    for (int r = 0; r < height; r++) {
        const int rowBase = r * width;
        for (int c = 0; c < width; c++) {
            float v = 0;
            for (int i = 0; i < ksize; i++) {
                const int cc = std::clamp(c + i - half, 0, width - 1);
                v += data[rowBase + cc] * k[i];
            }
            tmp[rowBase + c] = v;
        }
    }
    // Vertical pass
    for (int c = 0; c < width; c++) {
        for (int r = 0; r < height; r++) {
            float v = 0;
            for (int i = 0; i < ksize; i++) {
                const int rr = std::clamp(r + i - half, 0, height - 1);
                v += tmp[rr * width + c] * k[i];
            }
            data[r * width + c] = v;
        }
    }
}

// 4-connected flood fill starting at (seedX, seedY). Returns visited mask and
// also writes the visited pixel count out-param.
std::vector<uint8_t> floodFill(const std::vector<uint8_t> &binary,
                               int width, int height,
                               int seedX, int seedY,
                               size_t &count) {
    std::vector<uint8_t> visited(binary.size(), 0);
    count = 0;
    if (seedX < 0 || seedX >= width || seedY < 0 || seedY >= height) return visited;
    if (!binary[seedY * width + seedX]) return visited;

    std::queue<std::pair<int, int>> q;
    q.push({seedX, seedY});
    visited[seedY * width + seedX] = 1;
    count = 1;
    const int dx[4] = {0, 0, -1, 1};
    const int dy[4] = {-1, 1, 0, 0};
    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop();
        for (int d = 0; d < 4; d++) {
            const int nx = cx + dx[d];
            const int ny = cy + dy[d];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t idx = static_cast<size_t>(ny) * width + nx;
            if (!binary[idx] || visited[idx]) continue;
            visited[idx] = 1;
            count++;
            q.push({nx, ny});
        }
    }
    return visited;
}

// Moore-neighbour boundary tracing for a single connected component mask.
// Returns the ordered pixel chain along the outer boundary.
std::vector<std::pair<int, int>> traceContour(const std::vector<uint8_t> &mask,
                                              int width, int height) {
    std::vector<std::pair<int, int>> contour;
    // Find the first boundary pixel (top-left scan).
    int sx = -1, sy = -1;
    for (int y = 0; y < height && sy < 0; y++) {
        for (int x = 0; x < width; x++) {
            if (mask[y * width + x]) { sx = x; sy = y; break; }
        }
    }
    if (sx < 0) return contour;

    // Directions: 0=E, 1=SE, 2=S, 3=SW, 4=W, 5=NW, 6=N, 7=NE (Moore)
    const int dx[8] = { 1, 1, 0,-1,-1,-1, 0, 1};
    const int dy[8] = { 0, 1, 1, 1, 0,-1,-1,-1};

    int cx = sx, cy = sy;
    int dir = 6; // starting backtrack direction = N
    contour.push_back({cx, cy});
    const size_t safetyCap = static_cast<size_t>(width) * height * 4 + 16;
    size_t steps = 0;

    while (steps++ < safetyCap) {
        // Look for next foreground neighbour starting at (dir+1) mod 8 ccw?
        // For Moore we rotate clockwise from the starting direction.
        int startDir = (dir + 6) & 7;  // -2 mod 8
        bool found = false;
        for (int k = 0; k < 8; k++) {
            const int d = (startDir + k) & 7;
            const int nx = cx + dx[d];
            const int ny = cy + dy[d];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            if (!mask[static_cast<size_t>(ny) * width + nx]) continue;
            cx = nx; cy = ny; dir = d; found = true;
            contour.push_back({cx, cy});
            break;
        }
        if (!found) break;
        // Termination: Jacob's stopping criterion — came back to start with same dir.
        if (cx == sx && cy == sy) break;
    }
    return contour;
}

// Perpendicular distance from point p to segment ab.
double perpDist(double px, double py, double ax, double ay, double bx, double by) {
    const double vx = bx - ax;
    const double vy = by - ay;
    const double len2 = vx * vx + vy * vy;
    if (len2 < 1e-18) {
        const double dx = px - ax, dy = py - ay;
        return std::sqrt(dx * dx + dy * dy);
    }
    const double t = ((px - ax) * vx + (py - ay) * vy) / len2;
    const double cx = ax + t * vx;
    const double cy = ay + t * vy;
    const double dx = px - cx, dy = py - cy;
    return std::sqrt(dx * dx + dy * dy);
}

// Douglas-Peucker polyline simplification (iterative).
std::vector<std::pair<double, double>> douglasPeucker(
    const std::vector<std::pair<double, double>> &pts, double eps) {
    const size_t n = pts.size();
    if (n < 3 || eps <= 0) return pts;

    std::vector<uint8_t> keep(n, 0);
    keep.front() = 1;
    keep.back() = 1;

    std::stack<std::pair<size_t, size_t>> stk;
    stk.push({0, n - 1});
    while (!stk.empty()) {
        auto [i, j] = stk.top();
        stk.pop();
        if (j <= i + 1) continue;
        double maxD = -1.0;
        size_t maxK = i;
        for (size_t k = i + 1; k < j; k++) {
            const double d = perpDist(pts[k].first, pts[k].second,
                                      pts[i].first, pts[i].second,
                                      pts[j].first, pts[j].second);
            if (d > maxD) { maxD = d; maxK = k; }
        }
        if (maxD > eps) {
            keep[maxK] = 1;
            stk.push({i, maxK});
            stk.push({maxK, j});
        }
    }

    std::vector<std::pair<double, double>> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) if (keep[i]) out.push_back(pts[i]);
    return out;
}

} // anonymous namespace

std::string detectStockpileJson(const std::string &rasterPath,
                                double lat, double lon,
                                double radiusMeters,
                                float sensitivity) {
    sensitivity = std::clamp(sensitivity, 0.0f, 1.0f);
    if (!(radiusMeters > 0.0))
        throw InvalidArgsException("radius must be positive");

    LOGD << "detectStockpile path=" << rasterPath
         << " lat=" << lat << " lon=" << lon
         << " r=" << radiusMeters << " s=" << sensitivity;

    // --- Open raster ---
    GDALDatasetH hDs = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (!hDs) throw AppException("Cannot open raster: " + rasterPath);
    DsGuard dsGuard{hDs};

    double gt[6];
    if (GDALGetGeoTransform(hDs, gt) != CE_None)
        throw AppException("Raster has no geotransform: " + rasterPath);

    const int rasterW = GDALGetRasterXSize(hDs);
    const int rasterH = GDALGetRasterYSize(hDs);
    if (GDALGetRasterCount(hDs) < 1)
        throw AppException("Raster has no bands");

    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(hBand, &hasNoData);
    const char *projRef = GDALGetProjectionRef(hDs);
    const std::string projection = projRef ? projRef : "";

    // --- CRS setup (WGS84 <-> raster, WGS84 <-> raster) ---
    SrsHandle wgs84(OSRNewSpatialReference(nullptr));
    OSRImportFromEPSG(wgs84.h, 4326);
    OSRSetAxisMappingStrategy(wgs84.h, OAMS_TRADITIONAL_GIS_ORDER);

    SrsHandle rasterSrs;
    bool rasterIsGeographic = false;
    if (!projection.empty()) {
        rasterSrs.h = OSRNewSpatialReference(nullptr);
        char *wktPtr = const_cast<char *>(projection.c_str());
        if (OSRImportFromWkt(rasterSrs.h, &wktPtr) != OGRERR_NONE) {
            OSRDestroySpatialReference(rasterSrs.h);
            rasterSrs.h = nullptr;
        } else {
            OSRSetAxisMappingStrategy(rasterSrs.h, OAMS_TRADITIONAL_GIS_ORDER);
            rasterIsGeographic = (OSRIsGeographic(rasterSrs.h) != 0);
        }
    }

    // Click coordinates in raster CRS.
    double clickX = lon, clickY = lat;
    if (rasterSrs.h) {
        CtHandle wgsToRaster;
        wgsToRaster.h = OCTNewCoordinateTransformation(wgs84.h, rasterSrs.h);
        if (!wgsToRaster.h || !OCTTransform(wgsToRaster.h, 1, &clickX, &clickY, nullptr))
            throw AppException("Cannot project click coordinates into raster CRS");
    }

    double invGt[6];
    if (!GDALInvGeoTransform(gt, invGt))
        throw AppException("Cannot invert geotransform");

    const double centerColD = invGt[0] + invGt[1] * clickX + invGt[2] * clickY;
    const double centerRowD = invGt[3] + invGt[4] * clickX + invGt[5] * clickY;
    const int centerCol = static_cast<int>(std::floor(centerColD));
    const int centerRow = static_cast<int>(std::floor(centerRowD));

    if (centerCol < 0 || centerCol >= rasterW || centerRow < 0 || centerRow >= rasterH)
        throw InvalidArgsException("Click point is outside raster bounds");

    // Convert radius (meters) to pixels. For projected CRS the pixel size is
    // already in meters; for geographic CRS we approximate using lat.
    double metersPerPixelX = std::fabs(gt[1]);
    double metersPerPixelY = std::fabs(gt[5]);
    if (rasterIsGeographic || !rasterSrs.h) {
        const double d2r = M_PI / 180.0;
        const double mPerDeg = 111320.0; // good-enough WGS84 approximation
        metersPerPixelX = std::fabs(gt[1]) * mPerDeg * std::cos(lat * d2r);
        metersPerPixelY = std::fabs(gt[5]) * mPerDeg;
        if (metersPerPixelX < 1e-6) metersPerPixelX = mPerDeg * std::fabs(gt[1]);
    }
    const double pixelMeters = 0.5 * (metersPerPixelX + metersPerPixelY);
    const int radiusPx = std::max(8, static_cast<int>(std::ceil(radiusMeters / pixelMeters)));

    const int startCol = std::max(0, centerCol - radiusPx);
    const int startRow = std::max(0, centerRow - radiusPx);
    const int endCol = std::min(rasterW, centerCol + radiusPx + 1);
    const int endRow = std::min(rasterH, centerRow + radiusPx + 1);
    const int width = endCol - startCol;
    const int height = endRow - startRow;
    if (width < 3 || height < 3)
        throw InvalidArgsException("Search window too small; increase the radius");

    // --- Read raster window ---
    std::vector<float> region(static_cast<size_t>(width) * height);
    if (GDALRasterIO(hBand, GF_Read, startCol, startRow, width, height,
                     region.data(), width, height, GDT_Float32, 0, 0) != CE_None)
        throw AppException("Cannot read raster data");

    const auto isValid = [&](float v) {
        if (!std::isfinite(v)) return false;
        if (hasNoData && std::fabs(static_cast<double>(v) - noData) < 1e-9) return false;
        return true;
    };

    // --- Base plane from outer border ring ---
    std::vector<double> border;
    border.reserve(static_cast<size_t>(width + height) * 2);
    for (int c = 0; c < width; c++) {
        if (isValid(region[c])) border.push_back(region[c]);
        if (isValid(region[(height - 1) * width + c])) border.push_back(region[(height - 1) * width + c]);
    }
    for (int r = 1; r < height - 1; r++) {
        if (isValid(region[r * width])) border.push_back(region[r * width]);
        if (isValid(region[r * width + width - 1])) border.push_back(region[r * width + width - 1]);
    }
    if (border.empty())
        throw AppException("Search region contains no valid data");

    double baseElev;
    if (sensitivity < 0.3f) {
        std::sort(border.begin(), border.end());
        baseElev = border[border.size() / 2]; // median
    } else {
        double s = 0;
        for (double v : border) s += v;
        baseElev = s / static_cast<double>(border.size());
    }

    // --- Difference map + smoothing ---
    std::vector<float> diff(static_cast<size_t>(width) * height, 0.0f);
    for (size_t i = 0; i < diff.size(); i++) {
        if (isValid(region[i])) diff[i] = region[i] - static_cast<float>(baseElev);
        else                    diff[i] = 0.0f;
    }

    const float sigma = (1.0f - sensitivity) * 10.0f + 1.0f;
    gaussianFilter(diff, width, height, sigma);

    // Adaptive threshold on |diff|.
    double meanAbs = 0.0;
    size_t validCnt = 0;
    for (float v : diff) {
        if (std::fabs(v) > 1e-9) { meanAbs += std::fabs(v); validCnt++; }
    }
    if (validCnt > 0) meanAbs /= static_cast<double>(validCnt);
    if (meanAbs < 1e-6) meanAbs = 1e-6;
    const double threshold = meanAbs * (1.5 - sensitivity);

    std::vector<uint8_t> binary(diff.size(), 0);
    for (size_t i = 0; i < diff.size(); i++)
        binary[i] = (diff[i] > threshold) ? 1 : 0;

    // --- Flood fill from click ---
    const int localCol = centerCol - startCol;
    const int localRow = centerRow - startRow;
    size_t componentSize = 0;
    auto component = floodFill(binary, width, height, localCol, localRow, componentSize);
    if (componentSize < 4) {
        // Try nudging the seed to the strongest neighbour within a small radius
        // (helpful when the click lands slightly off the pile).
        int bestX = localCol, bestY = localRow;
        float bestV = -std::numeric_limits<float>::max();
        const int nudge = std::min(8, std::min(width, height) / 4);
        for (int dy = -nudge; dy <= nudge; dy++) {
            for (int dx = -nudge; dx <= nudge; dx++) {
                const int nx = localCol + dx, ny = localRow + dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const float v = diff[ny * width + nx];
                if (binary[ny * width + nx] && v > bestV) { bestV = v; bestX = nx; bestY = ny; }
            }
        }
        component = floodFill(binary, width, height, bestX, bestY, componentSize);
    }
    if (componentSize < 4)
        throw AppException("No stockpile detected at this location");

    // --- Trace contour and simplify ---
    auto contourPx = traceContour(component, width, height);
    if (contourPx.size() < 3)
        throw AppException("Failed to trace stockpile contour");

    // Convert pixels to raster map coordinates.
    std::vector<std::pair<double, double>> contourMap;
    contourMap.reserve(contourPx.size());
    for (const auto &p : contourPx) {
        const double mx = gt[0] + (startCol + p.first + 0.5) * gt[1]
                          + (startRow + p.second + 0.5) * gt[2];
        const double my = gt[3] + (startCol + p.first + 0.5) * gt[4]
                          + (startRow + p.second + 0.5) * gt[5];
        contourMap.push_back({mx, my});
    }

    // Douglas-Peucker in map units. Tune epsilon by sensitivity:
    // low sensitivity -> coarser polygon, high sensitivity -> finer polygon.
    const double epsPx = (1.0 - sensitivity) * 5.0 + 0.5;
    auto simplified = douglasPeucker(contourMap, epsPx * pixelMeters);
    if (simplified.size() < 3) simplified = contourMap;

    // Close the polygon ring.
    if (simplified.front() != simplified.back()) simplified.push_back(simplified.front());

    // Back to WGS84 for GeoJSON output.
    std::vector<std::pair<double, double>> ring;
    ring.reserve(simplified.size());
    if (rasterSrs.h) {
        CtHandle rasterToWgs;
        rasterToWgs.h = OCTNewCoordinateTransformation(rasterSrs.h, wgs84.h);
        if (!rasterToWgs.h)
            throw AppException("Cannot build transform to WGS84");
        for (auto &p : simplified) {
            double x = p.first, y = p.second;
            if (!OCTTransform(rasterToWgs.h, 1, &x, &y, nullptr))
                throw AppException("Cannot transform contour to WGS84");
            ring.push_back({x, y}); // lon, lat (traditional GIS order)
        }
    } else {
        ring = simplified; // already WGS84
    }

    // --- Preliminary volume estimate inside the component ---
    const double pixelArea = metersPerPixelX * metersPerPixelY;
    double estVolume = 0.0;
    for (size_t i = 0; i < component.size(); i++) {
        if (component[i] && diff[i] > 0) estVolume += diff[i] * pixelArea;
    }

    // Confidence: scale by shape compactness + component size.
    const double areaPx = static_cast<double>(componentSize);
    const double boundaryPx = static_cast<double>(contourPx.size());
    const double compactness = (boundaryPx > 0)
        ? std::min(1.0, (4.0 * M_PI * areaPx) / (boundaryPx * boundaryPx))
        : 0.0;
    const double windowArea = static_cast<double>(width) * height;
    const double fillRatio = std::min(1.0, areaPx / windowArea);
    const double confidence = std::clamp(0.4 + 0.4 * compactness + 0.2 * fillRatio, 0.0, 1.0);

    // --- Build GeoJSON polygon ---
    json coords = json::array();
    json outerRing = json::array();
    for (const auto &p : ring) {
        outerRing.push_back({p.first, p.second}); // [lon, lat]
    }
    coords.push_back(outerRing);
    json polygon = { {"type", "Polygon"}, {"coordinates", coords} };

    json result;
    result["polygon"] = polygon;
    result["estimatedVolume"] = estVolume;
    result["confidence"] = confidence;
    result["baseElevation"] = baseElev;
    result["basePlaneMethod"] = "auto";
    result["searchRadius"] = radiusMeters;
    result["sensitivityUsed"] = sensitivity;
    result["pixelCount"] = static_cast<uint64_t>(componentSize);

    LOGD << "Stockpile: volume=" << estVolume
         << " confidence=" << confidence
         << " pixels=" << componentSize;

    return result.dump();
}

} // namespace ddb
