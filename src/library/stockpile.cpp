/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "stockpile.h"
#include "cancellation.h"
#include "dem.h"
#include "mathutil.h"
#include "plane_fit.h"
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
    double reach = 0;       // meters: the distance from the click that the window covers in every direction
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
        throw NoRasterDataException("Click point is outside raster bounds");

    // The radius (meters) in pixels of each axis, with the size of a pixel on the ground at the
    // click: the window holds the disc of the radius on the ground (at least 8 pixels each way)
    w.pixel = dem.pixelSizeMeters(clickX, clickY);
    w.pixelMeters = 0.5 * (w.pixel.x + w.pixel.y);
    const int radiusCols = std::max(8, static_cast<int>(std::ceil(radiusMeters / w.pixel.x)));
    const int radiusRows = std::max(8, static_cast<int>(std::ceil(radiusMeters / w.pixel.y)));

    w.startCol = std::max(0, w.centerCol - radiusCols);
    w.startRow = std::max(0, w.centerRow - radiusRows);
    const int endCol = std::min(dem.width, w.centerCol + radiusCols + 1);
    const int endRow = std::min(dem.height, w.centerRow + radiusRows + 1);
    w.width = endCol - w.startCol;
    w.height = endRow - w.startRow;
    w.reach = std::min(radiusCols * w.pixel.x, radiusRows * w.pixel.y);
    if (w.width < 3 || w.height < 3)
        throw RefusedArgumentException("Search window too small; increase the radius");
    return w;
}

// The smoothed difference from the base, and where it exceeds the threshold
struct DifferenceMap {
    std::vector<float> diff;
    std::vector<uint8_t> binary;
};

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

// The elevations of the search window, on the ground around the click: x east of it and y south
// of it, in meters (the window has square pixels on the ground only in a projected CRS)
struct ClickGrid {
    const DemContext &dem;
    const std::vector<float> &z;
    int width = 0, height = 0;
    int col = 0, row = 0;   // the click
    PixelSize pixel;
    double pixelMeters = 0;

    double x(int c) const { return (c - col) * pixel.x; }
    double y(int r) const { return (r - row) * pixel.y; }
    double distance(int c, int r) const { return std::hypot(x(c), y(r)); }
    float at(int c, int r) const { return z[static_cast<size_t>(r) * width + c]; }
    bool valid(int c, int r) const { return dem.isValid(at(c, r)); }
};

// The pixels of the window within `reach` meters of the click on both axes
struct Box {
    int col0 = 0, row0 = 0; // in the window
    int width = 0, height = 0;

    size_t index(int c, int r) const { return static_cast<size_t>(r) * width + c; }
    size_t size() const { return static_cast<size_t>(width) * height; }
};

Box boxAround(const ClickGrid &g, double reach) {
    const int halfCols = static_cast<int>(std::ceil(reach / g.pixel.x));
    const int halfRows = static_cast<int>(std::ceil(reach / g.pixel.y));
    Box b;
    b.col0 = std::max(0, g.col - halfCols);
    b.row0 = std::max(0, g.row - halfRows);
    b.width = std::min(g.width, g.col + halfCols + 1) - b.col0;
    b.height = std::min(g.height, g.row + halfRows + 1) - b.row0;
    return b;
}

// Where a pile rises above a base plane fitted to the ground around it
struct PileCandidate {
    Box box;
    Plane base;                 // in the meters of the ClickGrid
    double threshold = 0;       // meters above the base
    std::vector<uint8_t> mask;  // over the box: the connected area of the click above the threshold
    size_t count = 0;
    double peak = 0;            // meters: the highest smoothed rise of the area
    double groundRms = 0;       // meters: how far the ground pixels are from the base (RMS of the ones it was fitted to)
    bool closed = false;        // the area stays within the limit of the measure and the box: the ground surrounds it
};

// The smallest rise above the base that counts, meters: the noise of a perfectly flat DEM is that
// of its float samples, far below
constexpr double kMinRelief = 0.01;

// Measures the pile of the click against the plane fitted to the `ground` pixels (box indices,
// valid ones) of the box. The rise is smoothed by a gaussian of `sigma` pixels, and counts where it
// exceeds the noise of the ground: twice its RMS at the default sensitivity, less at a higher one.
// Only the pixels closer to the click than `limit` meters belong to the pile; it is closed when it
// stays more than a pixel and a half within that limit, and off the edges of the box.
PileCandidate measurePile(const ClickGrid &g, const Box &box, const std::vector<size_t> &ground,
                          double limit, float sigma, float sensitivity) {
    PileCandidate p;
    p.box = box;

    PlanePoints points;
    points.x.reserve(ground.size());
    points.y.reserve(ground.size());
    points.z.reserve(ground.size());
    for (size_t i : ground) {
        const int c = box.col0 + static_cast<int>(i % static_cast<size_t>(box.width));
        const int r = box.row0 + static_cast<int>(i / static_cast<size_t>(box.width));
        points.add(g.x(c), g.y(r), g.at(c, r));
    }
    const RobustPlane fit = robustPlaneFit(points);
    p.base = fit.plane;
    p.groundRms = fit.rms;

    DifferenceMap map;
    map.diff.assign(box.size(), 0.0f);
    for (int r = 0; r < box.height; r++) {
        for (int c = 0; c < box.width; c++) {
            const int wc = box.col0 + c, wr = box.row0 + r;
            if (g.valid(wc, wr))
                map.diff[box.index(c, r)] = static_cast<float>(g.at(wc, wr) - p.base.at(g.x(wc), g.y(wr)));
        }
    }
    gaussianFilter(map.diff, box.width, box.height, sigma);

    // The noise of the ground: the smoothed rise of the ground pixels the plane was fitted to
    double sumSq = 0.0;
    size_t n = 0;
    const double inlierLimit = 2.0 * fit.rms;
    for (size_t k = 0; k < ground.size(); k++) {
        if (std::fabs(points.z[k] - p.base.at(points.x[k], points.y[k])) > inlierLimit) continue;
        const double d = map.diff[ground[k]];
        sumSq += d * d;
        n++;
    }
    const double noise = n > 0 ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;
    p.threshold = std::max(kMinRelief, 2.0 * (1.5 - sensitivity) * noise);

    map.binary.assign(box.size(), 0);
    for (int r = 0; r < box.height; r++) {
        for (int c = 0; c < box.width; c++) {
            const size_t i = box.index(c, r);
            map.binary[i] = map.diff[i] > p.threshold && g.distance(box.col0 + c, box.row0 + r) < limit ? 1 : 0;
        }
    }

    p.mask = componentAtClick(map, box.width, box.height, g.col - box.col0, g.row - box.row0, p.count);
    if (p.count == 0) return p;

    const double edge = limit - 1.5 * g.pixelMeters;
    p.closed = true;
    for (int r = 0; r < box.height; r++) {
        for (int c = 0; c < box.width; c++) {
            const size_t i = box.index(c, r);
            if (!p.mask[i]) continue;
            p.peak = std::max(p.peak, static_cast<double>(map.diff[i]));
            if (c == 0 || r == 0 || c == box.width - 1 || r == box.height - 1 ||
                g.distance(box.col0 + c, box.row0 + r) >= edge)
                p.closed = false;
        }
    }
    return p;
}

// A pile: big enough to have an outline, and rising clearly above the noise of its ground
bool isPile(const PileCandidate &p) {
    return p.count >= 4 && p.closed && p.peak >= 2.0 * p.threshold;
}

// The valid pixels of the box between `inner` and `outer` meters from the click
std::vector<size_t> ringPixels(const ClickGrid &g, const Box &box, double inner, double outer) {
    std::vector<size_t> ring;
    for (int r = 0; r < box.height; r++) {
        for (int c = 0; c < box.width; c++) {
            const int wc = box.col0 + c, wr = box.row0 + r;
            const double d = g.distance(wc, wr);
            if (d >= inner && d < outer && g.valid(wc, wr)) ring.push_back(box.index(c, r));
        }
    }
    return ring;
}

// The valid pixels of the box outside the mask, within `width` pixels of it
std::vector<size_t> bandAround(const ClickGrid &g, const Box &box, const std::vector<uint8_t> &mask, int width) {
    std::vector<uint8_t> grown = mask;
    std::vector<uint8_t> next;
    for (int step = 0; step < width; step++) {
        next = grown;
        for (int r = 0; r < box.height; r++) {
            for (int c = 0; c < box.width; c++) {
                if (grown[box.index(c, r)]) continue;
                bool touches = false;
                for (int dr = -1; dr <= 1 && !touches; dr++) {
                    for (int dc = -1; dc <= 1; dc++) {
                        const int nc = c + dc, nr = r + dr;
                        if (nc < 0 || nr < 0 || nc >= box.width || nr >= box.height) continue;
                        if (grown[box.index(nc, nr)]) { touches = true; break; }
                    }
                }
                if (touches) next[box.index(c, r)] = 1;
            }
        }
        grown.swap(next);
    }
    std::vector<size_t> band;
    for (int r = 0; r < box.height; r++) {
        for (int c = 0; c < box.width; c++) {
            const size_t i = box.index(c, r);
            if (grown[i] && !mask[i] && g.valid(box.col0 + c, box.row0 + r)) band.push_back(i);
        }
    }
    return band;
}

// How much the ring descends outwards from the base plane fitted to it, in meters per meter: the
// slope of the residuals of its pixels against their distance from the click. Nothing on ground,
// whatever its tilt (the plane takes the tilt); the slope of the flank when the ring is on a pile.
double ringDescent(const ClickGrid &g, const Box &box, const std::vector<size_t> &ring, const PileCandidate &p) {
    double sumD = 0, sumR = 0;
    size_t n = 0;
    std::vector<std::pair<double, double>> samples; // distance, residual
    samples.reserve(ring.size());
    for (size_t i : ring) {
        const int c = box.col0 + static_cast<int>(i % static_cast<size_t>(box.width));
        const int r = box.row0 + static_cast<int>(i / static_cast<size_t>(box.width));
        const double res = g.at(c, r) - p.base.at(g.x(c), g.y(r));
        if (std::fabs(res) > 2.0 * p.groundRms) continue;
        const double d = g.distance(c, r);
        samples.emplace_back(d, res);
        sumD += d; sumR += res; n++;
    }
    if (n < 3) return 0.0;
    const double meanD = sumD / static_cast<double>(n), meanR = sumR / static_cast<double>(n);
    double sxy = 0, sxx = 0;
    for (const auto &[d, res] : samples) {
        sxy += (d - meanD) * (res - meanR);
        sxx += (d - meanD) * (d - meanD);
    }
    return sxx > 0 ? -sxy / sxx : 0.0;
}

// The pile of the click. The ground around it is a ring around the click, fitted with a plane:
// on a slope that plane is the slope, so what rises above it is the pile and not the slope.
// The ring grows from a few pixels. While it is on the pile, the area above its plane is the top
// of the pile, and the ring descends outwards about as steeply as the pile does from that top to
// it. The first ring that does not, with a pile inside it that rises clearly above the noise of the
// ring and does not cross it, is the ground around the pile. Its base is then fitted again to the
// ground just around the outline of the pile: the base a best-fit volume of that outline has.
// Throws NoStockpileException when no ring of the window surrounds a pile.
PileCandidate findPile(const ClickGrid &g, double reach, float sensitivity) {
    auto ringWidth = [&](double r) { return std::max(2.0 * g.pixelMeters, 0.25 * r); };
    // The smoothing of the rise: wider at a lower sensitivity, but no wider than a third of the
    // ring, or it spreads the pile over the ring that should be its ground
    const float baseSigma = (1.0f - sensitivity) * 10.0f + 1.0f;
    auto smoothing = [&](double r) {
        return std::min(baseSigma, std::max(1.0f, static_cast<float>(ringWidth(r) / g.pixelMeters / 3.0)));
    };

    std::optional<PileCandidate> chosen;
    double chosenRadius = 0;
    // The rings grow by a quarter, and the last one is the largest that the window holds
    const double lastRadius = std::min(reach / 1.25, reach - 2.0 * g.pixelMeters);
    auto nextRadius = [&](double r) {
        const double next = std::max(r * 1.25, r + g.pixelMeters);
        return next > lastRadius && r < lastRadius ? lastRadius : next;
    };
    for (double r = 4.0 * g.pixelMeters; r + ringWidth(r) <= reach + 1e-9; r = nextRadius(r)) {
        cancel::throwIfCanceled();
        const double w = ringWidth(r);
        const Box box = boxAround(g, r + w);
        const std::vector<size_t> ring = ringPixels(g, box, r, r + w);
        // A ring mostly off the raster or over nodata says little about the ground
        const double expected = kPi * ((r + w) * (r + w) - r * r) / (g.pixel.x * g.pixel.y);
        if (ring.size() < 8 || static_cast<double>(ring.size()) < 0.2 * expected) continue;
        PileCandidate current = measurePile(g, box, ring, r + w, smoothing(r), sensitivity);
        const double descent = ringDescent(g, box, ring, current);
        LOGD << "stockpile ring r=" << r << " count=" << current.count << " closed=" << current.closed
             << " peak=" << current.peak << " threshold=" << current.threshold << " descent=" << descent
             << " flank=" << current.peak / r;
        // The ring is on the ground when it descends much less than the pile does from its top to it
        if (isPile(current) && descent <= 0.25 * current.peak / r) {
            chosen = std::move(current);
            chosenRadius = r;
            break;
        }
    }
    if (!chosen)
        throw NoStockpileException("No stockpile detected at this location");

    // The base fitted to the ground just around the outline of the pile
    const int bandWidth = std::clamp(static_cast<int>(std::lround(0.125 * chosenRadius / g.pixelMeters)), 2, 10);
    const float sigma = smoothing(chosenRadius);
    for (int it = 0; it < 3; it++) {
        const std::vector<size_t> band = bandAround(g, chosen->box, chosen->mask, bandWidth);
        if (band.size() < 8) break;
        PileCandidate refined = measurePile(g, chosen->box, band, std::numeric_limits<double>::infinity(),
                                            sigma, sensitivity);
        LOGD << "stockpile refined count=" << refined.count << " closed=" << refined.closed
             << " peak=" << refined.peak << " threshold=" << refined.threshold;
        if (!isPile(refined)) break;
        const double change = std::fabs(static_cast<double>(refined.count) - static_cast<double>(chosen->count))
                              / static_cast<double>(chosen->count);
        chosen = std::move(refined);
        if (change < 0.02) break;
    }
    return std::move(*chosen);
}

// The Douglas-Peucker tolerance, in map units, of a distance of `pixels` pixels of `groundPixel`
// meters. The map units of a projected raster are its own: a distance of pixels is that many of its
// pixels, however the projection stretches the ground. A geographic or CRS-less raster has always
// taken the tolerance in meters, whatever its units.
double simplifyTolerance(const DemContext &dem, double pixels, double groundPixel) {
    if (dem.isGeographic || !dem.srs)
        return pixels * groundPixel;
    return pixels * 0.5 * (std::fabs(dem.gt[1]) + std::fabs(dem.gt[5]));
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
    const std::vector<float> region = dem.readElevations(win.startCol, win.startRow, win.width, win.height);
    if (std::none_of(region.begin(), region.end(), [&](float v) { return dem.isValid(v); }))
        throw NoRasterDataException("Search region contains no valid data");

    // --- The pile, measured against the ground around it ---
    const ClickGrid grid{dem, region, win.width, win.height, win.centerCol - win.startCol,
                         win.centerRow - win.startRow, win.pixel, win.pixelMeters};
    const PileCandidate pile = findPile(grid, win.reach, sensitivity);
    const Box &box = pile.box;

    // --- Trace contour and simplify ---
    auto contourPx = traceContour(pile.mask, box.width, box.height);
    if (contourPx.size() < 3)
        throw NoStockpileException("No stockpile detected at this location: its contour has fewer than 3 points");

    // Convert pixels to raster map coordinates.
    Ring contourMap;
    contourMap.reserve(contourPx.size());
    for (const auto &p : contourPx) {
        const double col = win.startCol + box.col0 + p.first + 0.5;
        const double row = win.startRow + box.row0 + p.second + 0.5;
        contourMap.push_back({gt[0] + col * gt[1] + row * gt[2], gt[3] + col * gt[4] + row * gt[5]});
    }

    // Douglas-Peucker in map units. Tune epsilon by sensitivity:
    // low sensitivity -> coarser polygon, high sensitivity -> finer polygon.
    const double epsPx = (1.0 - sensitivity) * 5.0 + 0.5;
    const Ring ring = wgs84Ring(dem, contourMap, simplifyTolerance(dem, epsPx, win.pixelMeters));

    // --- Volume above the base the pile was found on, and the mean elevation of that base ---
    const double pixelArea = win.pixel.x * win.pixel.y;
    double estVolume = 0.0;
    double baseSum = 0.0;
    for (int r = 0; r < box.height; r++) {
        for (int c = 0; c < box.width; c++) {
            if (!pile.mask[box.index(c, r)]) continue;
            const int wc = box.col0 + c, wr = box.row0 + r;
            const double base = pile.base.at(grid.x(wc), grid.y(wr));
            baseSum += base;
            if (grid.valid(wc, wr)) estVolume += std::max(0.0, grid.at(wc, wr) - base) * pixelArea;
        }
    }
    const double baseElev = baseSum / static_cast<double>(pile.count);

    // Confidence: scale by shape compactness + component size.
    const double areaPx = static_cast<double>(pile.count);
    const double compactness = compactnessOf(areaPx, static_cast<double>(contourPx.size()));
    const double windowArea = static_cast<double>(win.width) * win.height;
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
    result["pixelCount"] = static_cast<uint64_t>(pile.count);

    LOGD << "Stockpile: volume=" << estVolume
         << " confidence=" << confidence
         << " pixels=" << pile.count;

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

    // Pixel size in meters (for area/volume), measured on the ground at the center of the raster
    const double centerX = dem.gt[0] + (rasterW / 2.0) * dem.gt[1] + (rasterH / 2.0) * dem.gt[2];
    const double centerY = dem.gt[3] + (rasterW / 2.0) * dem.gt[4] + (rasterH / 2.0) * dem.gt[5];
    ScaledRaster r;
    r.pixel = dem.pixelSizeMeters(centerX, centerY);
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

// The terrain trend: at every cell, the least-squares plane of the elevations around it, each
// weighted by a gaussian of its distance. A tilted surface is its own trend, up to the edges of
// the data and around nodata too, where a smoothing of the elevations bends it towards whatever
// fills the gaps: on a hillside, the high edge of the data stood out as a pile. The planes are
// fitted to blocks of cells, a block being the mean of its valid elevations at their centroid
// (exact for a plane), and blended bilinearly between the blocks.
std::vector<float> terrainTrend(const DemContext &dem, const ScaledRaster &raster, float sensitivity) {
    const std::vector<float> &region = raster.samples;
    const int width = raster.width, height = raster.height;

    // Sigma scales with image size and inversely with sensitivity:
    // bigger sigma -> smoother trend -> more features come out.
    const float baseSigma = std::max(8.0f, static_cast<float>(std::min(width, height)) / 16.0f);
    const double sigmaTrend = baseSigma * (1.5f - sensitivity);
    const int block = std::max(1, static_cast<int>(sigmaTrend / 3.0));
    const int blocksX = (width + block - 1) / block, blocksY = (height + block - 1) / block;
    auto blockIndex = [&](int bx, int by) { return static_cast<size_t>(by) * blocksX + bx; };

    // The valid elevations of every block: how many, and their sums of x, y and z
    struct BlockSums { double n = 0, x = 0, y = 0, z = 0; };
    std::vector<BlockSums> blocks(static_cast<size_t>(blocksX) * blocksY);
    bool anyValid = false;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t i = static_cast<size_t>(y) * width + x;
            const float v = region[i];
            if (!dem.isValid(v)) continue;
            anyValid = true;
            BlockSums &b = blocks[blockIndex(x / block, y / block)];
            b.n += 1; b.x += x + 0.5; b.y += y + 0.5; b.z += v;
        }
    }
    if (!anyValid)
        throw NoRasterDataException("Raster contains no valid elevation data");

    // The plane of every block, around its centre
    struct BlockPlane { Plane plane; bool valid = false; };
    std::vector<BlockPlane> planes(blocks.size());
    const int reach = static_cast<int>(std::ceil(3.0 * sigmaTrend / block));
    auto centre = [&](int b) { return (b + 0.5) * block; };
    for (int by = 0; by < blocksY; by++) {
        cancel::throwIfCanceled();
        for (int bx = 0; bx < blocksX; bx++) {
            PlaneSums sums;
            for (int ny = std::max(0, by - reach); ny <= std::min(blocksY - 1, by + reach); ny++) {
                for (int nx = std::max(0, bx - reach); nx <= std::min(blocksX - 1, bx + reach); nx++) {
                    const BlockSums &b = blocks[blockIndex(nx, ny)];
                    if (b.n <= 0) continue;
                    const double dx = b.x / b.n - centre(bx), dy = b.y / b.n - centre(by);
                    const double w = b.n * std::exp(-(dx * dx + dy * dy) / (2.0 * sigmaTrend * sigmaTrend));
                    sums.add(dx, dy, b.z / b.n, w);
                }
            }
            BlockPlane &bp = planes[blockIndex(bx, by)];
            if (!(sums.n > 0)) continue;
            bp.valid = true;
            if (!solvePlane(sums, bp.plane)) bp.plane = Plane{0.0, 0.0, sums.z / sums.n};
        }
    }

    // Every cell: the planes of the four blocks around it, blended by their distance
    std::vector<float> trend(region.size(), 0.0f);
    for (int y = 0; y < height; y++) {
        cancel::throwIfCanceled();
        const double fy = (y + 0.5) / block - 0.5;
        const int by0 = static_cast<int>(std::floor(fy));
        const double ty = fy - by0;
        for (int x = 0; x < width; x++) {
            const double fx = (x + 0.5) / block - 0.5;
            const int bx0 = static_cast<int>(std::floor(fx));
            const double tx = fx - bx0;
            double sum = 0, weights = 0;
            for (int k = 0; k < 4; k++) {
                const int bx = std::clamp(bx0 + (k & 1), 0, blocksX - 1);
                const int by = std::clamp(by0 + (k >> 1), 0, blocksY - 1);
                const BlockPlane &bp = planes[blockIndex(bx, by)];
                if (!bp.valid) continue;
                const double w = ((k & 1) ? tx : 1.0 - tx) * ((k >> 1) ? ty : 1.0 - ty);
                sum += w * bp.plane.at(x + 0.5 - centre(bx), y + 0.5 - centre(by));
                weights += w;
            }
            const size_t i = static_cast<size_t>(y) * width + x;
            trend[i] = weights > 0 ? static_cast<float>(sum / weights) : region[i];
        }
    }
    return trend;
}

// The rise of the terrain above its trend, smoothed lightly against noise, and how much of it
// counts: the mean of the positive rise, less at a higher sensitivity
struct Relief {
    std::vector<float> trend;
    std::vector<float> diff;
    double meanPositive = 0.0;
    double threshold = 0.0;
};

Relief reliefAbove(const DemContext &dem, const ScaledRaster &raster, std::vector<float> trend, float sensitivity) {
    const std::vector<float> &region = raster.samples;
    Relief r;
    r.trend = std::move(trend);
    r.diff.assign(region.size(), 0.0f);
    for (size_t i = 0; i < region.size(); i++) {
        if (dem.isValid(region[i])) r.diff[i] = region[i] - r.trend[i];
    }
    const float sigmaDiff = (1.0f - sensitivity) * 4.0f + 0.5f;
    gaussianFilter(r.diff, raster.width, raster.height, sigmaDiff);

    double sumPos = 0.0;
    size_t cntPos = 0;
    for (float v : r.diff) {
        if (v > 0.0f) { sumPos += v; cntPos++; }
    }
    if (cntPos == 0) return r;
    r.meanPositive = sumPos / static_cast<double>(cntPos);
    r.threshold = r.meanPositive * (1.5 - sensitivity);
    return r;
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
          epsMeters(simplifyTolerance(demContext, ((1.0 - sensitivity) * 4.0 + 0.5) * scaled.scale,
                                      0.5 * (scaled.pixel.x + scaled.pixel.y))),
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

    // --- The rise of the terrain above its trend ---
    const Relief relief = reliefAbove(dem, raster, terrainTrend(dem, raster, sensitivity), sensitivity);
    if (!(relief.meanPositive > 0))
        return emptyDetection(sensitivity, minAreaM2).dump();
    const std::vector<float> &trend = relief.trend;
    const std::vector<float> &diff = relief.diff;
    const double meanPos = relief.meanPositive;
    const double threshold = relief.threshold;
    LOGD << "detectAll: mean rise=" << meanPos << " threshold=" << threshold;

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
