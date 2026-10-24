/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stockpile.h"
#include "cancellation.h"
#include "dem.h"
#include "mathutil.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "json.h"
#include "logger.h"

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
#include <optional>
#include <queue>
#include <stack>
#include <string>
#include <vector>

namespace ddb {

namespace {

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
        cancel::throwIfCanceled();
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
        cancel::throwIfCanceled();
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
    size_t steps = 0;
    while (!q.empty()) {
        // A checkpoint every 64 Ki pixels: a fill can cover a whole raster
        if ((++steps & 0xFFFF) == 0)
            cancel::throwIfCanceled();
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
        // Termination: Jacob's stopping criterion - came back to start with same dir.
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

namespace {

using Ring = std::vector<std::pair<double, double>>;

// The GeoJSON polygon of a ring of (lon, lat) points
json polygonJson(const Ring &ring) {
    json coords = json::array();
    json outerRing = json::array();
    for (const auto &p : ring) {
        outerRing.push_back({p.first, p.second}); // [lon, lat]
    }
    coords.push_back(outerRing);
    return { {"type", "Polygon"}, {"coordinates", coords} };
}

// How close to a disc the shape is: 1 for a circle, towards 0 for a thin one
double compactnessOf(double areaPx, double boundaryPx) {
    return (boundaryPx > 0)
        ? std::min(1.0, (4.0 * ddb::kPi * areaPx) / (boundaryPx * boundaryPx))
        : 0.0;
}

// The window of the raster around a click, where the search for a pile runs
struct SearchWindow {
    int centerCol = 0, centerRow = 0;
    int startCol = 0, startRow = 0;
    int width = 0, height = 0;
    PixelSize pixel;        // meters
    double pixelMeters = 0; // the mean of both sides
};

SearchWindow searchWindow(const DemContext &dem, double lat, double lon, double radiusMeters) {
    const double *gt = dem.gt;

    // --- Click coordinates in raster CRS ---
    double clickX = lon, clickY = lat;
    if (const auto wgsToRaster = dem.wgs84ToDem()) {
        if (!wgsToRaster->tryTransform(1, &clickX, &clickY))
            throw AppException("Cannot project click coordinates into raster CRS");
    }

    double invGt[6];
    if (!GDALInvGeoTransform(gt, invGt))
        throw AppException("Cannot invert geotransform");

    SearchWindow w;
    const double centerColD = invGt[0] + invGt[1] * clickX + invGt[2] * clickY;
    const double centerRowD = invGt[3] + invGt[4] * clickX + invGt[5] * clickY;
    w.centerCol = static_cast<int>(std::floor(centerColD));
    w.centerRow = static_cast<int>(std::floor(centerRowD));

    if (w.centerCol < 0 || w.centerCol >= dem.width || w.centerRow < 0 || w.centerRow >= dem.height)
        throw InvalidArgsException("Click point is outside raster bounds");

    // Convert radius (meters) to pixels. For projected CRS the pixel size is
    // already in meters; for geographic CRS we approximate using lat.
    w.pixel = dem.pixelSizeMeters(lat);
    w.pixelMeters = 0.5 * (w.pixel.x + w.pixel.y);
    const int radiusPx = std::max(8, static_cast<int>(std::ceil(radiusMeters / w.pixelMeters)));

    w.startCol = std::max(0, w.centerCol - radiusPx);
    w.startRow = std::max(0, w.centerRow - radiusPx);
    const int endCol = std::min(dem.width, w.centerCol + radiusPx + 1);
    const int endRow = std::min(dem.height, w.centerRow + radiusPx + 1);
    w.width = endCol - w.startCol;
    w.height = endRow - w.startRow;
    if (w.width < 3 || w.height < 3)
        throw InvalidArgsException("Search window too small; increase the radius");
    return w;
}

// The elevation of the ground around the pile: the median (low sensitivity) or the mean of the
// valid samples of the outer border ring of the region
double borderElevation(const DemContext &dem, const std::vector<float> &region, int width, int height,
                       float sensitivity) {
    std::vector<double> border;
    border.reserve(static_cast<size_t>(width + height) * 2);
    for (int c = 0; c < width; c++) {
        if (dem.isValid(region[c])) border.push_back(region[c]);
        if (dem.isValid(region[(height - 1) * width + c])) border.push_back(region[(height - 1) * width + c]);
    }
    for (int r = 1; r < height - 1; r++) {
        if (dem.isValid(region[r * width])) border.push_back(region[r * width]);
        if (dem.isValid(region[r * width + width - 1])) border.push_back(region[r * width + width - 1]);
    }
    if (border.empty())
        throw AppException("Search region contains no valid data");

    if (sensitivity < 0.3f) {
        std::sort(border.begin(), border.end());
        return border[border.size() / 2]; // median
    }
    double s = 0;
    for (double v : border) s += v;
    return s / static_cast<double>(border.size());
}

// The smoothed difference from the base elevation, and where it exceeds the adaptive threshold
struct DifferenceMap {
    std::vector<float> diff;
    std::vector<uint8_t> binary;
};

DifferenceMap differenceMap(const DemContext &dem, const std::vector<float> &region, int width, int height,
                            double baseElev, float sensitivity) {
    DifferenceMap map;
    std::vector<float> &diff = map.diff;
    diff.assign(static_cast<size_t>(width) * height, 0.0f);
    for (size_t i = 0; i < diff.size(); i++) {
        if (dem.isValid(region[i])) diff[i] = region[i] - static_cast<float>(baseElev);
        else                        diff[i] = 0.0f;
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

    map.binary.assign(diff.size(), 0);
    for (size_t i = 0; i < diff.size(); i++)
        map.binary[i] = (diff[i] > threshold) ? 1 : 0;
    return map;
}

// The connected component of the click (local coordinates); when it is tiny, the click is
// nudged to the strongest neighbour of the difference map
std::vector<uint8_t> componentAtClick(const DifferenceMap &map, int width, int height, int localCol,
                                      int localRow, size_t &componentSize) {
    auto component = floodFill(map.binary, width, height, localCol, localRow, componentSize);
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
                const float v = map.diff[ny * width + nx];
                if (map.binary[ny * width + nx] && v > bestV) { bestV = v; bestX = nx; bestY = ny; }
            }
        }
        component = floodFill(map.binary, width, height, bestX, bestY, componentSize);
    }
    return component;
}

// Simplifies the contour (map coordinates) with Douglas-Peucker, closes the ring and moves it to
// WGS84 (lon, lat)
Ring wgs84Ring(const DemContext &dem, const Ring &contourMap, double epsilon) {
    auto simplified = douglasPeucker(contourMap, epsilon);
    if (simplified.size() < 3) simplified = contourMap;

    // Close the polygon ring.
    if (simplified.front() != simplified.back()) simplified.push_back(simplified.front());

    // Back to WGS84 for GeoJSON output.
    Ring ring;
    ring.reserve(simplified.size());
    if (const auto rasterToWgs = dem.demToWgs84()) {
        for (auto &p : simplified) {
            double x = p.first, y = p.second;
            if (!rasterToWgs->tryTransform(1, &x, &y))
                throw AppException("Cannot transform contour to WGS84");
            ring.push_back({x, y}); // lon, lat (traditional GIS order)
        }
    } else {
        ring = simplified; // already WGS84
    }
    return ring;
}

} // anonymous namespace

std::string detectStockpileJson(const std::string &rasterPath,
                                double lat, double lon,
                                double radiusMeters,
                                float sensitivity) {
    cancel::throwIfCanceled();
    sensitivity = std::clamp(sensitivity, 0.0f, 1.0f);
    if (!(radiusMeters > 0.0))
        throw InvalidArgsException("radius must be positive");

    LOGD << "detectStockpile path=" << rasterPath
         << " lat=" << lat << " lon=" << lon
         << " r=" << radiusMeters << " s=" << sensitivity;

    // --- Open raster, and the window around the click ---
    const DemContext dem = openDem(rasterPath);
    const double *gt = dem.gt;
    const SearchWindow win = searchWindow(dem, lat, lon, radiusMeters);
    const int width = win.width;
    const int height = win.height;

    // --- Read raster window ---
    const std::vector<float> region = dem.readElevations(win.startCol, win.startRow, width, height);

    // --- Base plane from outer border ring, difference map + smoothing ---
    const double baseElev = borderElevation(dem, region, width, height, sensitivity);
    const DifferenceMap map = differenceMap(dem, region, width, height, baseElev, sensitivity);
    const std::vector<float> &diff = map.diff;

    // --- Flood fill from click ---
    const int localCol = win.centerCol - win.startCol;
    const int localRow = win.centerRow - win.startRow;
    size_t componentSize = 0;
    const auto component = componentAtClick(map, width, height, localCol, localRow, componentSize);
    if (componentSize < 4)
        throw AppException("No stockpile detected at this location");

    // --- Trace contour and simplify ---
    auto contourPx = traceContour(component, width, height);
    if (contourPx.size() < 3)
        throw AppException("Failed to trace stockpile contour");

    // Convert pixels to raster map coordinates.
    Ring contourMap;
    contourMap.reserve(contourPx.size());
    for (const auto &p : contourPx) {
        const double mx = gt[0] + (win.startCol + p.first + 0.5) * gt[1]
                          + (win.startRow + p.second + 0.5) * gt[2];
        const double my = gt[3] + (win.startCol + p.first + 0.5) * gt[4]
                          + (win.startRow + p.second + 0.5) * gt[5];
        contourMap.push_back({mx, my});
    }

    // Douglas-Peucker in map units. Tune epsilon by sensitivity:
    // low sensitivity -> coarser polygon, high sensitivity -> finer polygon.
    const double epsPx = (1.0 - sensitivity) * 5.0 + 0.5;
    const Ring ring = wgs84Ring(dem, contourMap, epsPx * win.pixelMeters);

    // --- Preliminary volume estimate inside the component ---
    const double pixelArea = win.pixel.x * win.pixel.y;
    double estVolume = 0.0;
    for (size_t i = 0; i < component.size(); i++) {
        if (component[i] && diff[i] > 0) estVolume += diff[i] * pixelArea;
    }

    // Confidence: scale by shape compactness + component size.
    const double areaPx = static_cast<double>(componentSize);
    const double compactness = compactnessOf(areaPx, static_cast<double>(contourPx.size()));
    const double windowArea = static_cast<double>(width) * height;
    const double fillRatio = std::min(1.0, areaPx / windowArea);
    const double confidence = std::clamp(0.4 + 0.4 * compactness + 0.2 * fillRatio, 0.0, 1.0);

    json result;
    result["polygon"] = polygonJson(ring);
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

// ============================================================================
// Batch detection (full-DEM scan)
// ============================================================================

namespace {

// Two-pass connected-components labeling (4-connected) on a binary mask.
// Returns a label image (0 = background) and the number of components.
// Uses union-find to merge equivalences during the first pass.
std::vector<int32_t> labelConnectedComponents(const std::vector<uint8_t> &binary,
                                              int width, int height,
                                              int32_t &numLabels) {
    std::vector<int32_t> labels(binary.size(), 0);
    std::vector<int32_t> parent;
    parent.push_back(0); // 1-indexed

    auto findRoot = [&](int32_t a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    auto unite = [&](int32_t a, int32_t b) {
        a = findRoot(a);
        b = findRoot(b);
        if (a == b) return;
        if (a < b) parent[b] = a; else parent[a] = b;
    };

    // First pass.
    for (int y = 0; y < height; y++) {
        cancel::throwIfCanceled();
        for (int x = 0; x < width; x++) {
            const size_t idx = static_cast<size_t>(y) * width + x;
            if (!binary[idx]) continue;
            int32_t left = (x > 0) ? labels[idx - 1] : 0;
            int32_t up   = (y > 0) ? labels[idx - width] : 0;
            if (left == 0 && up == 0) {
                int32_t newLabel = static_cast<int32_t>(parent.size());
                parent.push_back(newLabel);
                labels[idx] = newLabel;
            } else if (left != 0 && up == 0) {
                labels[idx] = left;
            } else if (left == 0 && up != 0) {
                labels[idx] = up;
            } else {
                labels[idx] = std::min(left, up);
                if (left != up) unite(left, up);
            }
        }
    }

    // Second pass: replace each label with its root and remap to dense ids.
    std::vector<int32_t> remap(parent.size(), 0);
    int32_t nextId = 0;
    for (size_t i = 0; i < labels.size(); i++) {
        if (labels[i] == 0) continue;
        int32_t root = findRoot(labels[i]);
        if (remap[root] == 0) remap[root] = ++nextId;
        labels[i] = remap[root];
    }
    numLabels = nextId;
    return labels;
}

// Extract a single-component mask from the labels image.
std::vector<uint8_t> extractComponentMask(const std::vector<int32_t> &labels,
                                          int32_t componentId) {
    std::vector<uint8_t> mask(labels.size(), 0);
    for (size_t i = 0; i < labels.size(); i++) {
        if (labels[i] == componentId) mask[i] = 1;
    }
    return mask;
}

} // anonymous namespace


namespace {

// The raster as the batch detection reads it: shrunk by a power of two when it is huge
struct ScaledRaster {
    int scale = 1;
    int width = 0, height = 0;   // of the shrunk grid
    PixelSize pixel;             // meters of a pixel of the original raster
    double cellArea = 0;         // m^2 of a cell of the shrunk grid
    std::vector<float> samples;
};

ScaledRaster readScaled(const DemContext &dem) {
    const int rasterW = dem.width;
    const int rasterH = dem.height;

    // Approx pixel size in meters (for area/volume). A geographic raster uses its center latitude
    // for the cosine factor.
    const double centerY = dem.gt[3] + (rasterH / 2.0) * dem.gt[5];
    ScaledRaster r;
    r.pixel = dem.pixelSizeMeters(centerY);
    const double pixelArea = r.pixel.x * r.pixel.y;
    if (!(pixelArea > 0))
        throw AppException("Cannot compute pixel area for raster");

    // --- Decide reading scale (downsample huge rasters) ---
    // Cap working buffer at ~16M pixels to avoid OOM/DoS.
    const size_t maxPixels = 16ull * 1024ull * 1024ull;
    while (static_cast<size_t>(rasterW / r.scale) * static_cast<size_t>(rasterH / r.scale) > maxPixels) {
        r.scale *= 2;
    }
    r.width = std::max(3, rasterW / r.scale);
    r.height = std::max(3, rasterH / r.scale);
    r.cellArea = (r.pixel.x * static_cast<double>(r.scale)) * (r.pixel.y * static_cast<double>(r.scale));

    LOGD << "detectAll: rasterW=" << rasterW << " rasterH=" << rasterH
         << " scale=" << r.scale << " width=" << r.width << " height=" << r.height;

    // --- Read full raster (downsampled if needed) ---
    r.samples = dem.readElevations(0, 0, rasterW, rasterH, r.width, r.height);
    return r;
}

// The terrain trend: a strong smoothing of the elevation, with nodata replaced by the median of
// the valid values to keep the smoothing stable
std::vector<float> terrainTrend(const DemContext &dem, const ScaledRaster &raster, float sensitivity) {
    const std::vector<float> &region = raster.samples;
    std::vector<float> trend(region.size(), 0.0f);

    std::vector<double> valid;
    valid.reserve(region.size() / 4);
    for (float v : region) if (dem.isValid(v)) valid.push_back(v);
    if (valid.empty())
        throw AppException("Raster contains no valid elevation data");
    std::sort(valid.begin(), valid.end());
    const float fillVal = static_cast<float>(valid[valid.size() / 2]);
    for (size_t i = 0; i < region.size(); i++) {
        trend[i] = dem.isValid(region[i]) ? region[i] : fillVal;
    }
    // Sigma scales with image size and inversely with sensitivity:
    // bigger sigma -> smoother trend -> more features come out.
    const float baseSigma = std::max(8.0f, std::min(raster.width, raster.height) / 16.0f);
    const float sigmaTrend = baseSigma * (1.5f - sensitivity);
    gaussianFilter(trend, raster.width, raster.height, sigmaTrend);
    return trend;
}

// What a connected component adds up to
struct ComponentStat {
    int32_t id = 0;
    size_t pixelCount = 0;
    double estVolume = 0.0;
    double sumX = 0.0; // pixel X centroid accumulator
    double sumY = 0.0;
    double maxDiff = 0.0;
};

std::vector<ComponentStat> componentStats(const std::vector<int32_t> &labels, const std::vector<float> &diff,
                                          int width, int height, int32_t numLabels, double cellArea) {
    std::vector<ComponentStat> stats(numLabels + 1);
    for (int32_t i = 0; i <= numLabels; i++) stats[i].id = i;

    for (int y = 0; y < height; y++) {
        cancel::throwIfCanceled();
        for (int x = 0; x < width; x++) {
            const size_t idx = static_cast<size_t>(y) * width + x;
            const int32_t lab = labels[idx];
            if (lab == 0) continue;
            auto &s = stats[lab];
            s.pixelCount++;
            const double d = diff[idx];
            if (d > 0.0) s.estVolume += d * cellArea;
            s.sumX += x;
            s.sumY += y;
            if (d > s.maxDiff) s.maxDiff = d;
        }
    }
    return stats;
}

json emptyDetection(float sensitivity, double minAreaM2) {
    json result;
    result["stockpiles"] = json::array();
    result["totalFound"] = 0;
    result["sensitivityUsed"] = sensitivity;
    result["minAreaUsed"] = minAreaM2;
    return result;
}

// A stockpile found by the batch detection
struct Detection {
    int32_t id;
    json entry;
    double estVolume;
};

// The batch detection of every component that qualifies: polygon in WGS84, confidence, centroid
class DetectionBuilder {
    const DemContext &dem;
    const ScaledRaster &raster;
    const std::vector<float> &trend;
    const std::vector<int32_t> &labels;
    double meanPos;
    double epsMeters;
    std::unique_ptr<CoordsTransformer> rasterToWgs;

    void pixelToMap(double px, double py, double &mx, double &my) const {
        // px, py are in the downsampled grid; convert to original raster coords.
        const double opx = px * static_cast<double>(raster.scale);
        const double opy = py * static_cast<double>(raster.scale);
        const double *gt = dem.gt;
        mx = gt[0] + opx * gt[1] + opy * gt[2];
        my = gt[3] + opx * gt[4] + opy * gt[5];
    }

    void mapToWgs(double mx, double my, double &lon, double &lat) const {
        lon = mx; lat = my;
        if (rasterToWgs) {
            if (!rasterToWgs->tryTransform(1, &lon, &lat))
                throw AppException("Cannot transform to WGS84");
        }
    }

public:
    DetectionBuilder(const DemContext &demContext, const ScaledRaster &scaled, const std::vector<float> &trendMap,
                     const std::vector<int32_t> &componentLabels, float sensitivity, double meanPositive)
        : dem(demContext), raster(scaled), trend(trendMap), labels(componentLabels), meanPos(meanPositive),
          epsMeters(((1.0 - sensitivity) * 4.0 + 0.5) * 0.5 *
                    (scaled.pixel.x * scaled.scale + scaled.pixel.y * scaled.scale)),
          // Coord transformer raster->WGS84 (reused for all components)
          rasterToWgs(demContext.demToWgs84()) {}

    // The entry of a component, or nothing when its outline cannot be traced
    std::optional<Detection> build(int32_t lab, const ComponentStat &s, double areaM2) const {
        const int width = raster.width;
        const int height = raster.height;
        auto mask = extractComponentMask(labels, lab);
        auto contourPx = traceContour(mask, width, height);
        if (contourPx.size() < 3) return std::nullopt;

        // Convert to map coords
        Ring contourMap;
        contourMap.reserve(contourPx.size());
        for (const auto &p : contourPx) {
            double mx, my;
            pixelToMap(p.first + 0.5, p.second + 0.5, mx, my);
            contourMap.push_back({mx, my});
        }
        auto simplified = douglasPeucker(contourMap, epsMeters);
        if (simplified.size() < 3) simplified = contourMap;
        if (simplified.front() != simplified.back()) simplified.push_back(simplified.front());

        // To WGS84
        Ring ring;
        ring.reserve(simplified.size());
        for (auto &p : simplified) {
            double lon, lat;
            mapToWgs(p.first, p.second, lon, lat);
            ring.push_back({lon, lat});
        }

        // Confidence: combine compactness and prominence.
        const double areaPx = static_cast<double>(s.pixelCount);
        const double compactness = compactnessOf(areaPx, static_cast<double>(contourPx.size()));
        const double prominence = std::min(1.0, s.maxDiff / std::max(1e-6, meanPos * 4.0));
        const double confidence = std::clamp(0.3 + 0.4 * compactness + 0.3 * prominence, 0.0, 1.0);

        // Centroid in WGS84.
        const double cxPx = s.sumX / static_cast<double>(s.pixelCount);
        const double cyPx = s.sumY / static_cast<double>(s.pixelCount);
        double cMx, cMy;
        pixelToMap(cxPx + 0.5, cyPx + 0.5, cMx, cMy);
        double cLon, cLat;
        mapToWgs(cMx, cMy, cLon, cLat);

        // Base elevation: average trend at component centroid (meters).
        const size_t cIdx = static_cast<size_t>(std::clamp<int>(static_cast<int>(cyPx), 0, height - 1)) * width
                          + std::clamp<int>(static_cast<int>(cxPx), 0, width - 1);
        const double baseElev = trend[cIdx];

        json entry;
        entry["polygon"] = polygonJson(ring);
        entry["estimatedVolume"] = s.estVolume;
        entry["confidence"] = confidence;
        entry["baseElevation"] = baseElev;
        entry["centroid"] = { cLon, cLat };
        entry["areaM2"] = areaM2;
        entry["pixelCount"] = static_cast<uint64_t>(s.pixelCount);

        return Detection{ lab, std::move(entry), s.estVolume };
    }
};

} // anonymous namespace

std::string detectAllStockpilesJson(const std::string &rasterPath,
                                    float sensitivity,
                                    double minAreaM2,
                                    int maxResults) {
    cancel::throwIfCanceled();
    sensitivity = std::clamp(sensitivity, 0.0f, 1.0f);
    if (minAreaM2 < 0.0) minAreaM2 = 0.0;
    if (maxResults <= 0) maxResults = 50;
    // Hard cap: anti-DoS protection.
    if (maxResults > 500) maxResults = 500;

    LOGD << "detectAllStockpiles path=" << rasterPath
         << " s=" << sensitivity << " minArea=" << minAreaM2
         << " maxResults=" << maxResults;

    // --- Open raster, shrunk and read ---
    const DemContext dem = openDem(rasterPath);
    const ScaledRaster raster = readScaled(dem);
    const int width = raster.width;
    const int height = raster.height;
    const std::vector<float> &region = raster.samples;

    // --- Global low-pass base plane ---
    // Strong smoothing of the elevation gives an approximation of the terrain trend.
    // Then diff = elevation - trend isolates the prominent positive features.
    const std::vector<float> trend = terrainTrend(dem, raster, sensitivity);

    // --- Difference map ---
    std::vector<float> diff(region.size(), 0.0f);
    for (size_t i = 0; i < region.size(); i++) {
        if (dem.isValid(region[i])) diff[i] = region[i] - trend[i];
    }

    // Light smoothing on diff to suppress noise.
    const float sigmaDiff = (1.0f - sensitivity) * 4.0f + 0.5f;
    gaussianFilter(diff, width, height, sigmaDiff);

    // Adaptive threshold on positive diff (we only care about mounds).
    double sumPos = 0.0;
    size_t cntPos = 0;
    for (float v : diff) {
        if (v > 0.0f) { sumPos += v; cntPos++; }
    }
    if (cntPos == 0)
        return emptyDetection(sensitivity, minAreaM2).dump();
    const double meanPos = sumPos / static_cast<double>(cntPos);
    const double threshold = meanPos * (1.5 - sensitivity);

    std::vector<uint8_t> binary(diff.size(), 0);
    for (size_t i = 0; i < diff.size(); i++) {
        if (diff[i] > threshold && dem.isValid(region[i])) binary[i] = 1;
    }

    // --- Connected components ---
    int32_t numLabels = 0;
    auto labels = labelConnectedComponents(binary, width, height, numLabels);
    LOGD << "detectAll: components=" << numLabels;

    if (numLabels == 0)
        return emptyDetection(sensitivity, minAreaM2).dump();

    // --- Compute per-component stats ---
    const auto stats = componentStats(labels, diff, width, height, numLabels, raster.cellArea);

    // --- For each qualifying component, build polygon + result entry ---
    const DetectionBuilder builder(dem, raster, trend, labels, sensitivity, meanPos);
    std::vector<Detection> detections;
    detections.reserve(numLabels);

    for (int32_t lab = 1; lab <= numLabels; lab++) {
        cancel::throwIfCanceled();
        const auto &s = stats[lab];
        const double areaM2 = static_cast<double>(s.pixelCount) * raster.cellArea;
        if (areaM2 < minAreaM2) continue;
        if (s.pixelCount < 4) continue;

        if (auto detection = builder.build(lab, s, areaM2))
            detections.push_back(std::move(*detection));
    }

    // Sort by estimated volume desc, truncate to maxResults.
    std::sort(detections.begin(), detections.end(),
              [](const Detection &a, const Detection &b) { return a.estVolume > b.estVolume; });
    if (static_cast<int>(detections.size()) > maxResults)
        detections.resize(static_cast<size_t>(maxResults));

    // Assign sequential ids.
    json arr = json::array();
    for (size_t i = 0; i < detections.size(); i++) {
        detections[i].entry["id"] = static_cast<int>(i);
        arr.push_back(std::move(detections[i].entry));
    }

    json result;
    result["stockpiles"] = arr;
    result["totalFound"] = static_cast<int>(arr.size());
    result["sensitivityUsed"] = sensitivity;
    result["minAreaUsed"] = minAreaM2;

    LOGD << "detectAll: returning " << arr.size() << " stockpiles";
    return result.dump();
}

} // namespace ddb
