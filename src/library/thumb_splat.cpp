/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

#include "build_layout.h"
#include "exceptions.h"
#include "json.h"
#include "mio.h"
#include "parallel.h"
#include "rad.h"
#include "splat_frame.h"
#include "thumb_internal.h"

#include "splat_math.h"

// SPZ (vendored, MIT) - decode .spz to render an on-demand splat thumbnail.
#include "load-spz.h"
#include "splat-types.h"

namespace ddb {

namespace {

// A cloud of splats in display space: `colors` holds RGB in [0,1] and `opacities` the activated
// opacity (NOT logits); `scales` are the standard deviations along the axes of each splat (linear,
// not the logarithm a file stores) and `rotations` the quaternions x, y, z, w that turn them
struct SplatPreviewInput {
    const std::vector<float>& positions;
    const std::vector<float>& colors;
    const std::vector<float>& opacities;
    const std::vector<float>& scales;
    const std::vector<float>& rotations;
    size_t count;
    std::string sourceLabel;
};

using Vec3 = std::array<double, 3>;

double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 scaled(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 plus(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 minus(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 unit(const Vec3& a) {
    const double n = std::sqrt(dot(a, a));
    return n > 0.0 ? scaled(a, 1.0 / n) : a;
}

// A splat whose position is NaN or infinite has no place in the thumbnail: it takes no part in
// the frame, and is not drawn (the conversion of its position to a pixel would be undefined)
bool hasFinitePosition(const std::vector<float>& positions, size_t i) {
    return std::isfinite(positions[i * 3]) && std::isfinite(positions[i * 3 + 1]) &&
           std::isfinite(positions[i * 3 + 2]);
}

Vec3 positionOf(const std::vector<float>& positions, size_t i) {
    return {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
}

double opacityOf(const SplatPreviewInput& cloud, size_t i) {
    return i < cloud.opacities.size() ? static_cast<double>(cloud.opacities[i]) : 1.0;
}

constexpr double Pi = 3.14159265358979323846;
double radians(double degrees) { return degrees * Pi / 180.0; }

// A splat more transparent than this takes no part, in the frame or in the picture
constexpr double MinSplatOpacity = 0.02;
// A pixel that a splat changes by less than one level of an 8-bit channel is left as it is
constexpr double MinPixelAlpha = 1.0 / 255.0;
// The ellipse of a splat is drawn out to this many standard deviations
constexpr double CutSigmas = 3.0;
// The low-pass added to the covariance of a splat on the screen: nothing is smaller than a pixel
constexpr double LowPassPixels = 0.3;

// The largest thumbnail rendered from a splat. The splats are composited a tile at a time, so the
// picture (4 bytes a pixel) is the only buffer as large as the picture: 64 MiB at this size.
constexpr int MaxSplatThumbSize = 4096;

void assureSplatThumbSize(int thumbSize) {
    if (thumbSize <= 0 || thumbSize > MaxSplatThumbSize)
        throw InvalidArgsException("thumbSize must be between 1 and " + std::to_string(MaxSplatThumbSize) +
                                   " for a Gaussian Splat, got " + std::to_string(thumbSize));
}

// The lowest and the highest of `values` once the outermost `share` of them at each end is left out
std::pair<double, double> trimmedRange(std::vector<double> values, double share) {
    const size_t trim = static_cast<size_t>(share * static_cast<double>(values.size() - 1));
    const size_t hi = values.size() - 1 - trim;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(trim), values.end());
    const double lo = values[trim];
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(hi), values.end());
    return {lo, values[hi]};
}

// The values of `splats` along `axis`
std::vector<double> along(const SplatPreviewInput& cloud, const std::vector<size_t>& splats, const Vec3& axis) {
    std::vector<double> values;
    values.reserve(splats.size());
    for (const size_t i : splats)
        values.push_back(dot(positionOf(cloud.positions, i), axis));
    return values;
}

// ---------------------------------------------------------------------------
// The subject
// ---------------------------------------------------------------------------
//
// A scene holds more than its subject: floaters, the far background its cameras saw past it. The
// picture is aimed at, and fitted to, the subject only: the densest part of the scene (the cameras
// went round it, so it has the detail), a site or a map as a whole when it is about as dense all
// over, and the whole room when the scene is one.

// The box the scene is in once the outermost 1% of the splats along each axis are left out
constexpr double ScenePercentile = 0.01;
// The density is taken on a grid of this many cells along the longest side of that box: the
// opacity in each cell, averaged over the 27 cells around it
constexpr int SubjectGridCells = 48;
// The subject is the cells around the densest one that are at least half as dense, seen with some of
// what is around it: the frame is twice as wide, and at least 3/8 as wide as the middle 80% of the
// scene (along the width of the picture; the outer 10% at each side may be floaters) ...
constexpr double SubjectDensityShare = 0.5;
constexpr double SubjectContextMargin = 2.0;
constexpr double SceneContextShare = 0.375;
constexpr double SceneContextPercentile = 0.1;
// ... unless the cells around the densest one that are at least a fifth as dense hold this much of
// the opacity of the scene: then the scene has no one subject, and they are the subject
constexpr double SiteDensityShare = 0.2;
constexpr double SiteOpacityShare = 0.5;
// A scene of fewer splats has no density to speak of: all of it is the subject
constexpr size_t MinSplatsForSubject = 1000;

struct Box {
    Vec3 lo, hi;
    bool contains(const Vec3& p) const {
        return p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1] && p[2] >= lo[2] && p[2] <= hi[2];
    }
};

Box sceneBox(const SplatPreviewInput& cloud, const std::vector<size_t>& splats) {
    Box box;
    for (int k = 0; k < 3; ++k) {
        Vec3 axis = {0, 0, 0};
        axis[k] = 1.0;
        std::tie(box.lo[k], box.hi[k]) = trimmedRange(along(cloud, splats, axis), ScenePercentile);
    }
    return box;
}

// The splats within the box
std::vector<size_t> within(const SplatPreviewInput& cloud, const std::vector<size_t>& splats, const Box& box) {
    std::vector<size_t> inside;
    inside.reserve(splats.size());
    for (const size_t i : splats)
        if (box.contains(positionOf(cloud.positions, i)))
            inside.push_back(i);
    return inside;
}

double opacitySum(const SplatPreviewInput& cloud, const std::vector<size_t>& splats) {
    double sum = 0.0;
    for (const size_t i : splats)
        sum += std::min(1.0, opacityOf(cloud, i));
    return sum;
}

// The density of the splats in a box, on a grid of cubic cells
class DensityGrid {
public:
    DensityGrid(const SplatPreviewInput& cloud, const std::vector<size_t>& inside, const Box& box)
        : cloud_(cloud), inside_(inside), box_(box) {
        const Vec3 extent = minus(box.hi, box.lo);
        cell_ = std::max({extent[0], extent[1], extent[2]}) / SubjectGridCells;
        if (!(cell_ > 0.0))
            return;
        for (int k = 0; k < 3; ++k)
            dims_[k] = std::clamp(static_cast<int>(std::ceil(extent[k] / cell_)), 1, SubjectGridCells);
        stride_ = {static_cast<size_t>(dims_[1]) * static_cast<size_t>(dims_[2]), static_cast<size_t>(dims_[2]), 1};
        const size_t cells = static_cast<size_t>(dims_[0]) * stride_[0];

        density_.assign(cells, 0.0f);
        for (const size_t i : inside)
            density_[cellOf(i)] += static_cast<float>(std::min(1.0, opacityOf(cloud, i)));

        // Summed over the 3x3x3 cells around each one, one axis at a time
        std::vector<float> pass(cells);
        for (int k = 0; k < 3; ++k) {
            for (size_t c = 0; c < cells; ++c) {
                const int at = coordinate(c, k);
                float sum = density_[c];
                if (at > 0) sum += density_[c - stride_[k]];
                if (at + 1 < dims_[k]) sum += density_[c + stride_[k]];
                pass[c] = sum;
            }
            density_.swap(pass);
        }
        peak_ = static_cast<size_t>(std::max_element(density_.begin(), density_.end()) - density_.begin());
    }

    bool usable() const { return !density_.empty(); }

    // The splats in the cells connected to the densest one, through their faces, by cells at least
    // `share` as dense as it
    std::vector<size_t> partAround(double share) const {
        const float threshold = static_cast<float>(share) * density_[peak_];
        std::vector<uint8_t> inPart(density_.size(), 0);
        std::vector<size_t> queue = {peak_};
        inPart[peak_] = 1;
        while (!queue.empty()) {
            const size_t c = queue.back();
            queue.pop_back();
            for (int k = 0; k < 3; ++k) {
                const int at = coordinate(c, k);
                for (const int step : {-1, 1}) {
                    if (at + step < 0 || at + step >= dims_[k])
                        continue;
                    const size_t next = step < 0 ? c - stride_[k] : c + stride_[k];
                    if (inPart[next] == 0 && density_[next] >= threshold) {
                        inPart[next] = 1;
                        queue.push_back(next);
                    }
                }
            }
        }

        std::vector<size_t> part;
        for (const size_t i : inside_)
            if (inPart[cellOf(i)] != 0)
                part.push_back(i);
        return part;
    }

private:
    size_t cellOf(size_t i) const {
        const Vec3 p = positionOf(cloud_.positions, i);
        size_t index = 0;
        for (int k = 0; k < 3; ++k)
            index += stride_[k] * static_cast<size_t>(
                                      std::clamp(static_cast<int>((p[k] - box_.lo[k]) / cell_), 0, dims_[k] - 1));
        return index;
    }
    int coordinate(size_t c, int k) const {
        return static_cast<int>((c / stride_[k]) % static_cast<size_t>(dims_[k]));
    }

    const SplatPreviewInput& cloud_;
    const std::vector<size_t>& inside_;
    Box box_;
    double cell_ = 0.0;
    std::array<int, 3> dims_{1, 1, 1};
    std::array<size_t, 3> stride_{1, 1, 1};
    std::vector<float> density_;
    size_t peak_ = 0;
};

// What the camera is aimed at and fitted to, and the room the frame leaves around it
struct Subject {
    std::vector<size_t> splats;
    double margin;
    bool inItsSurroundings = false; // a subject the scene has more of around it, shown with some of it
};

// The frame leaves this much room around the subject when it is not seen with its surroundings
constexpr double FrameMargin = 1.08;

Subject findSubject(const SplatPreviewInput& cloud, const std::vector<size_t>& inside, const Box& box) {
    if (inside.size() < MinSplatsForSubject)
        return {inside, FrameMargin};
    const DensityGrid grid(cloud, inside, box);
    if (!grid.usable())
        return {inside, FrameMargin};
    std::vector<size_t> site = grid.partAround(SiteDensityShare);
    if (opacitySum(cloud, site) >= SiteOpacityShare * opacitySum(cloud, inside))
        return {std::move(site), FrameMargin};
    return {grid.partAround(SubjectDensityShare), SubjectContextMargin, true};
}

// A room, or any scene the cameras were inside of: from the middle of the scene there are splats
// on both sides along its flattest axis (a floor and a ceiling), where the open air has the sky on
// one side and the ground under it on the other. Measured as the opacity in a cone of 45 degrees
// around each of the two directions, beyond 15% of the radius of the scene (the 90th percentile of
// the distances), against what an even spread over all directions puts in such a cone.
constexpr double EnclosureConeDegrees = 45.0;
constexpr double EnclosureInnerRadius = 0.15;
constexpr double EnclosedShare = 0.1;
// The test looks at this many splats at most
constexpr size_t EnclosureSample = 40000;

bool isEnclosed(const SplatPreviewInput& cloud, const std::vector<size_t>& inside) {
    if (inside.size() < MinSplatsForSubject)
        return false;
    const size_t step = std::max<size_t>(1, inside.size() / EnclosureSample);
    std::vector<Vec3> points;
    std::vector<double> weights;
    for (size_t j = 0; j < inside.size(); j += step) {
        points.push_back(positionOf(cloud.positions, inside[j]));
        weights.push_back(std::min(1.0, opacityOf(cloud, inside[j])));
    }

    // The middle: the median along each axis
    Vec3 middle{};
    for (int k = 0; k < 3; ++k) {
        std::vector<double> values;
        values.reserve(points.size());
        for (const Vec3& p : points)
            values.push_back(p[k]);
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
        middle[k] = values[values.size() / 2];
    }
    const Vec3 flattest = principalAxes(points, weights).axes[2];

    std::vector<double> distances;
    distances.reserve(points.size());
    for (const Vec3& p : points)
        distances.push_back(std::sqrt(dot(minus(p, middle), minus(p, middle))));
    const size_t at90 = distances.size() * 9 / 10;
    std::vector<double> sorted = distances;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(at90), sorted.end());
    const double inner = EnclosureInnerRadius * sorted[at90];

    const double cosCone = std::cos(radians(EnclosureConeDegrees));
    double total = 0.0, above = 0.0, below = 0.0;
    for (size_t j = 0; j < points.size(); ++j) {
        if (!(distances[j] > inner))
            continue;
        total += weights[j];
        const double c = dot(minus(points[j], middle), flattest) / distances[j];
        if (c > cosCone) above += weights[j];
        if (c < -cosCone) below += weights[j];
    }
    if (!(total > 0.0))
        return false;
    const double evenShare = (1.0 - cosCone) / 2.0;
    return std::min(above, below) / total >= EnclosedShare * evenShare;
}

// ---------------------------------------------------------------------------
// The camera
// ---------------------------------------------------------------------------

// A pinhole camera: its axes in the frame of the splats, as seen by someone looking along `forward`
// with `up` towards the top of the picture
struct CameraAxes {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

// The up a scene is seen with when estimateSplatUp is not sure of one: the viewer's, -Y (the frame
// of COLMAP and INRIA scenes), so the thumbnail shows the scene as the viewer opens it
constexpr Vec3 DefaultUp = {0.0, -1.0, 0.0};
// Vertical field of view of the thumbnail; a long lens: the splats keep their proportions
constexpr double FieldOfViewDegrees = 40.0;
// How the three-quarter view stands: above the ground by the elevation (higher over a room, to see
// into it), and turned from the axis along which the subject is narrowest by the azimuth, towards
// the one along which it is longest
constexpr double ElevationDegrees = 35.0;
constexpr double EnclosedElevationDegrees = 55.0;
constexpr double AzimuthDegrees = 35.0;

// Looking along `forward`, up the picture being `up` as far as it can be (forward is never
// parallel to it)
CameraAxes cameraLooking(const Vec3& forward, const Vec3& up) {
    CameraAxes axes;
    axes.forward = unit(forward);
    axes.right = unit(cross(axes.forward, up));
    axes.up = cross(axes.right, axes.forward);
    return axes;
}

// A unit vector at right angles to `n`
Vec3 anyPerpendicular(const Vec3& n) {
    const int least = std::abs(n[0]) <= std::abs(n[1]) && std::abs(n[0]) <= std::abs(n[2]) ? 0
                      : std::abs(n[1]) <= std::abs(n[2])                                  ? 1
                                                                                          : 2;
    Vec3 axis = {0, 0, 0};
    axis[least] = 1.0;
    return unit(cross(n, axis));
}

// The three-quarter view of a subject that stands on `up`: the picture is wide along the longest
// direction of its ground, and the camera looks down on it from `elevation` degrees
CameraAxes threeQuarterView(const SplatPreviewInput& cloud, const std::vector<size_t>& subject, const Vec3& up,
                            double elevationDegrees) {
    const Vec3 e1 = anyPerpendicular(up);
    const Vec3 e2 = cross(up, e1);

    // The spread of the splats over the ground: the axes of its 2x2 covariance
    double m1 = 0, m2 = 0;
    for (const size_t i : subject) {
        const Vec3 p = positionOf(cloud.positions, i);
        m1 += dot(p, e1);
        m2 += dot(p, e2);
    }
    m1 /= static_cast<double>(subject.size());
    m2 /= static_cast<double>(subject.size());
    double c11 = 0, c12 = 0, c22 = 0;
    for (const size_t i : subject) {
        const Vec3 p = positionOf(cloud.positions, i);
        const double a = dot(p, e1) - m1, b = dot(p, e2) - m2;
        c11 += a * a;
        c12 += a * b;
        c22 += b * b;
    }
    // The major axis of the ground, at `angle` from e1
    const double angle = 0.5 * std::atan2(2.0 * c12, c11 - c22);
    Vec3 major = plus(scaled(e1, std::cos(angle)), scaled(e2, std::sin(angle)));
    Vec3 minor = cross(up, major);
    // The sign of an axis is the same for the same scene: its largest component is positive
    const auto canonical = [](Vec3 v) {
        const int big = std::abs(v[0]) >= std::abs(v[1]) && std::abs(v[0]) >= std::abs(v[2]) ? 0
                        : std::abs(v[1]) >= std::abs(v[2])                                  ? 1
                                                                                            : 2;
        return v[big] < 0 ? scaled(v, -1.0) : v;
    };
    major = canonical(major);
    minor = canonical(minor);

    const double elevation = radians(elevationDegrees);
    const double azimuth = radians(AzimuthDegrees);
    const Vec3 ground = plus(scaled(minor, std::cos(azimuth)), scaled(major, std::sin(azimuth)));
    const Vec3 toCamera = plus(scaled(ground, std::cos(elevation)), scaled(up, std::sin(elevation)));
    return cameraLooking(scaled(toCamera, -1.0), up);
}

// Where the camera stands: it looks at the middle of the subject, and is far enough for all of it
// but its outermost splats to be in the picture
struct Framing {
    double cx, cy, cz; // the middle of the subject, along the axes of the camera
    double distance;   // from the camera to the middle of the subject
    double depth;      // of the subject, along the line of sight
    double half;       // half the width of the picture, at the middle of the subject
};

// The share of the subject's splats the frame leaves out at each end of each axis
constexpr double FramePercentile = 0.02;

// The frame of `subject` with `margin` around it, half as wide as `minHalf` at least
Framing frameSubject(const SplatPreviewInput& cloud, const std::vector<size_t>& subject, double margin,
                     double minHalf, const CameraAxes& camera, double tanHalf) {
    const auto [xLo, xHi] = trimmedRange(along(cloud, subject, camera.right), FramePercentile);
    const auto [yLo, yHi] = trimmedRange(along(cloud, subject, camera.up), FramePercentile);
    const auto [zLo, zHi] = trimmedRange(along(cloud, subject, camera.forward), FramePercentile);

    Framing frame{(xLo + xHi) / 2, (yLo + yHi) / 2, (zLo + zHi) / 2, 0.0, zHi - zLo, 0.0};
    double half = std::max(xHi - xLo, yHi - yLo) / 2;
    if (!(half > 0.0)) {
        // The centres are on one line of sight: what the picture can show is the splats themselves
        for (const size_t i : subject)
            for (int k = 0; k < 3; ++k) {
                const double v = cloud.scales.size() >= (i + 1) * 3 ? cloud.scales[i * 3 + k] : 0.0;
                if (std::isfinite(v)) half = std::max(half, CutSigmas * v);
            }
        if (!(half > 0.0))
            throw AppException("Gaussian Splat has zero planar extent; cannot render thumbnail");
    }
    frame.half = std::max(half * margin, minHalf);
    frame.distance = frame.half / tanHalf + frame.depth / 2;
    return frame;
}

// What is not drawn around a subject in the open: what stands between the camera and the subject (a
// tree, a floater: it would hide the subject and is no part of it; a margin of the subject's depth
// keeps its outermost splats), the far background (beyond the subject by more than this share of the
// larger of its depth and the width of the picture), the sky (higher above the subject than the
// subject stands above the ground under it) and what is under that ground (deeper than a tenth of
// that: with the ground in front of the subject cut away, the splats under it would show). The ground is the lowest 1% of the splats within the width of the picture of the subject.
constexpr double NearMargin = 0.05;
constexpr double FarShare = 1.5;
constexpr double SkyShare = 1.0;
constexpr double UnderGroundShare = 0.1;
constexpr double GroundPercentile = 0.01;
// Of a room: its top (above this share of its height) and its near side (in front of the middle by
// this share of its depth), as a doll's house is opened, so the picture shows the inside
constexpr double RoomTopShare = 0.6;
constexpr double RoomNearShare = 0.15;
// Nothing nearer the camera than this share of its distance to the subject is drawn
constexpr double NearShare = 0.1;
// A splat whose footprint (out to 3 sigma) is wider than this share of the picture is not drawn: on a
// coarse level of detail it is a blur over the picture, not a part of the subject. (In a picture of a
// few pixels, a footprint of this many pixels is not too wide.)
constexpr double MaxFootprintShare = 0.125;
constexpr double MaxFootprintFloorPixels = 8.0;

// What is cut away: nearer the camera (z), farther from it, higher (along up) and lower
struct Cuts {
    double nearZ;
    double farZ = std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double floor = -std::numeric_limits<double>::infinity();
};

Cuts roomCuts(const SplatPreviewInput& cloud, const std::vector<size_t>& room, const Vec3& up, const Framing& frame) {
    Cuts cuts;
    cuts.nearZ = std::max(NearShare * frame.distance, frame.distance + (RoomNearShare - 0.5) * frame.depth);
    const auto [lo, hi] = trimmedRange(along(cloud, room, up), FramePercentile);
    cuts.top = lo + RoomTopShare * (hi - lo);
    return cuts;
}

Cuts openAirCuts(const SplatPreviewInput& cloud, const std::vector<size_t>& inside, const std::vector<size_t>& subject,
                 const Vec3& up, const CameraAxes& camera, const Framing& frame) {
    Cuts cuts;
    cuts.nearZ = std::max(NearShare * frame.distance, frame.distance - (0.5 + NearMargin) * frame.depth);
    cuts.farZ = frame.distance + 0.5 * frame.depth + FarShare * std::max(frame.depth, 2.0 * frame.half);

    // The ground under the subject: the lowest of the splats around it
    const Vec3 middle = plus(plus(scaled(camera.right, frame.cx), scaled(camera.up, frame.cy)), scaled(camera.forward, frame.cz));
    std::vector<double> around;
    for (const size_t i : inside) {
        const Vec3 offset = minus(positionOf(cloud.positions, i), middle);
        const Vec3 lateral = minus(offset, scaled(up, dot(offset, up)));
        if (dot(lateral, lateral) <= frame.half * frame.half)
            around.push_back(dot(offset, up) + dot(middle, up));
    }
    const auto [lo, hi] = trimmedRange(along(cloud, subject, up), FramePercentile);
    const double ground = around.empty() ? lo : std::min(lo, trimmedRange(std::move(around), GroundPercentile).first);
    // A subject at one height, on no ground of its own, keeps a twentieth of the width of the picture above it
    const double standing = std::max(hi - ground, 0.1 * frame.half);
    cuts.top = hi + SkyShare * standing;
    cuts.floor = ground - UnderGroundShare * standing;
    return cuts;
}

// ---------------------------------------------------------------------------
// The projection
// ---------------------------------------------------------------------------

// What the compositing needs of a splat once it is in the picture
struct ProjectedSplat {
    float depth;
    float x, y;             // centre, in pixels
    float a, b, c;          // inverse of the 2D covariance (the conic): a dx^2 + 2 b dx dy + c dy^2
    float radiusX, radiusY; // half the extent of its 3 sigma ellipse
    float opacity;
    float minPower;         // the exponent below which it changes no pixel
    float r, g, bl;
};

// The shape of a splat on the screen, by the EWA projection of its covariance: the Jacobian of the
// perspective projection at its centre, times the rotation into the camera, times its own shape
// (R S). `x`, `y` and `z` are its coordinates in the camera.
bool projectSplat(const SplatPreviewInput& cloud, size_t i, const CameraAxes& camera, double x, double y,
                  double z, double focal, double tanHalf, ProjectedSplat& out) {
    // The shape: R S, where S holds the standard deviations and R is the quaternion as a matrix
    double s[3] = {0, 0, 0};
    if (cloud.scales.size() >= (i + 1) * 3)
        for (int k = 0; k < 3; ++k) {
            const double v = cloud.scales[i * 3 + k];
            s[k] = std::isfinite(v) && v > 0.0 ? v : 0.0;
        }
    double q[4] = {0, 0, 0, 1};
    if (cloud.rotations.size() >= (i + 1) * 4) {
        double norm = 0;
        for (int k = 0; k < 4; ++k) {
            q[k] = cloud.rotations[i * 4 + k];
            norm += q[k] * q[k];
        }
        if (std::isfinite(norm) && norm > 0.0) {
            norm = std::sqrt(norm);
            for (double& v : q) v /= norm;
        } else {
            q[0] = q[1] = q[2] = 0, q[3] = 1;
        }
    }
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const double R[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                            {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                            {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};

    // The shape in the camera: W R S, W having the axes of the camera as rows
    const Vec3* axes[3] = {&camera.right, &camera.up, &camera.forward};
    double C[3][3];
    for (int j = 0; j < 3; ++j)
        for (int c = 0; c < 3; ++c)
            C[j][c] = ((*axes[j])[0] * R[0][c] + (*axes[j])[1] * R[1][c] + (*axes[j])[2] * R[2][c]) * s[c];

    // Where the Jacobian is taken: kept within the view, as the splats beside the frame are
    // stretched by the perspective out of proportion
    const double limit = 1.3 * tanHalf;
    const double jx = std::clamp(x / z, -limit, limit) * z;
    const double jy = std::clamp(y / z, -limit, limit) * z;
    const double invZ = focal / z, invZ2 = focal / (z * z);

    double sxx = LowPassPixels, sxy = 0, syy = LowPassPixels;
    for (int c = 0; c < 3; ++c) {
        const double u = invZ * C[0][c] - invZ2 * jx * C[2][c];
        const double v = -invZ * C[1][c] + invZ2 * jy * C[2][c]; // pixel rows grow downwards
        sxx += u * u;
        sxy += u * v;
        syy += v * v;
    }
    const double det = sxx * syy - sxy * sxy;
    if (!(det > 0.0) || !std::isfinite(det))
        return false;

    out.a = static_cast<float>(syy / det);
    out.b = static_cast<float>(-sxy / det);
    out.c = static_cast<float>(sxx / det);
    out.radiusX = static_cast<float>(CutSigmas * std::sqrt(sxx));
    out.radiusY = static_cast<float>(CutSigmas * std::sqrt(syy));
    return true;
}

// ---------------------------------------------------------------------------
// The picture
// ---------------------------------------------------------------------------
//
// The splats are composited front to back, a tile of the picture at a time: each tile keeps the
// colour gathered so far and the transmittance (how much of what is behind still shows) of its
// pixels, a pixel is done once almost nothing behind it shows, and a tile once all its pixels are.
// The sum is the one back-to-front compositing makes (the colour of each splat, times its alpha,
// times the transmittance of the splats in front of it), but the splats hidden behind the first
// opaque ones are never painted, and only the tiles being painted have buffers of their own.

constexpr int TilePixels = 16;
// A pixel is done once less than this of what is behind it shows
constexpr float DoneTransmittance = 1e-4f;
// No splat is quite opaque: its edge always lets some of what is behind it through
constexpr float MaxAlpha = 0.99f;
// The tiles are painted on this many threads at most (CPU tokens permitting)
constexpr size_t PaintThreads = 4;

// Paints the splats of each tile (`lists[offsets[t]..offsets[t + 1]]`, nearest first) into `image`
void paintTiles(const std::vector<ProjectedSplat>& splats, const std::vector<uint32_t>& offsets,
                const std::vector<uint32_t>& lists, int tilesX, int size, RasterImage& image) {
    const size_t pixels = image.pixels();
    const size_t tiles = offsets.size() - 1;
    parallel::forEachIndex(tiles, PaintThreads, [&](size_t t) {
        const int tx0 = static_cast<int>(t % static_cast<size_t>(tilesX)) * TilePixels;
        const int ty0 = static_cast<int>(t / static_cast<size_t>(tilesX)) * TilePixels;
        const int width = std::min(TilePixels, size - tx0);
        const int height = std::min(TilePixels, size - ty0);

        std::array<float, TilePixels * TilePixels * 3> colour{};
        std::array<float, TilePixels * TilePixels> transmittance;
        transmittance.fill(1.0f);
        int done = 0;
        const int all = width * height;

        for (uint32_t k = offsets[t]; k < offsets[t + 1] && done < all; ++k) {
            const ProjectedSplat& splat = splats[lists[k]];
            const int x0 = std::max(tx0, static_cast<int>(std::floor(splat.x - splat.radiusX)));
            const int x1 = std::min(tx0 + width - 1, static_cast<int>(std::ceil(splat.x + splat.radiusX)));
            const int y0 = std::max(ty0, static_cast<int>(std::floor(splat.y - splat.radiusY)));
            const int y1 = std::min(ty0 + height - 1, static_cast<int>(std::ceil(splat.y + splat.radiusY)));
            for (int py = y0; py <= y1; ++py) {
                const float dy = static_cast<float>(py) + 0.5f - splat.y;
                const int row = (py - ty0) * TilePixels;
                for (int px = x0; px <= x1; ++px) {
                    const int at = row + px - tx0;
                    const float through = transmittance[static_cast<size_t>(at)];
                    if (through < DoneTransmittance)
                        continue;
                    const float dx = static_cast<float>(px) + 0.5f - splat.x;
                    const float power = -0.5f * (splat.a * dx * dx + 2.0f * splat.b * dx * dy + splat.c * dy * dy);
                    if (power < splat.minPower)
                        continue;
                    const float alpha = std::min(MaxAlpha, splat.opacity * std::exp(power));
                    if (alpha < static_cast<float>(MinPixelAlpha))
                        continue;
                    const float weight = through * alpha;
                    float* c = &colour[static_cast<size_t>(at) * 3];
                    c[0] += weight * splat.r;
                    c[1] += weight * splat.g;
                    c[2] += weight * splat.bl;
                    const float left = through * (1.0f - alpha);
                    transmittance[static_cast<size_t>(at)] = left;
                    if (left < DoneTransmittance)
                        ++done;
                }
            }
        }

        // Band-sequential RGB (3 planes) + a separate alpha plane, as in RasterImage
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px) {
                const size_t at = static_cast<size_t>(py * TilePixels + px);
                const float coverage = 1.0f - transmittance[at];
                if (coverage < static_cast<float>(MinPixelAlpha))
                    continue;
                const size_t pixel = static_cast<size_t>(ty0 + py) * static_cast<size_t>(size) + static_cast<size_t>(tx0 + px);
                for (int k = 0; k < 3; ++k)
                    image.color[static_cast<size_t>(k) * pixels + pixel] = static_cast<uint8_t>(
                        std::lround(std::clamp(colour[at * 3 + static_cast<size_t>(k)] / coverage, 0.0f, 1.0f) * 255.0f));
                image.alpha[pixel] = static_cast<uint8_t>(std::lround(std::min(coverage, 1.0f) * 255.0f));
            }
    });
}

// The splats that take part: a position that is a number, and some opacity (a NaN has none)
std::vector<size_t> participating(const SplatPreviewInput& cloud) {
    const size_t n = cloud.count;
    if (n == 0 || cloud.positions.size() < n * 3)
        throw AppException("No splats to render in " + cloud.sourceLabel);

    std::vector<size_t> splats;
    bool anyFinite = false;
    for (size_t i = 0; i < n; ++i) {
        if (!hasFinitePosition(cloud.positions, i))
            continue;
        anyFinite = true;
        if (opacityOf(cloud, i) >= MinSplatOpacity)
            splats.push_back(i);
    }
    if (!anyFinite)
        throw AppException("Gaussian Splat has no finite position; cannot render thumbnail");
    if (splats.empty())
        throw AppException("No splats projected into the thumbnail frame");
    return splats;
}

// How the scene is seen, whatever the size of the picture: the camera, where it stands and what is
// cut away, in the frame of the splats
struct SplatView {
    Vec3 up;
    CameraAxes camera;
    Framing frame;
    Cuts cuts;
    bool enclosed;
    size_t framed;
};

const double TanHalfFieldOfView = std::tan(radians(FieldOfViewDegrees / 2.0));

// The camera stands the scene on `up` (estimateSplatUp's, when it is sure of it; the viewer's
// default otherwise) and looks down on the subject in three-quarter view, from outside it. A room is
// seen from higher up, opened like a doll's house.
SplatView chooseView(const SplatPreviewInput& cloud, const std::vector<size_t>& splats, const SplatUp& estimatedUp) {
    const Box scene = sceneBox(cloud, splats);
    const std::vector<size_t> inside = within(cloud, splats, scene);
    const bool enclosed = isEnclosed(cloud, inside);
    const Subject found = enclosed ? Subject{inside, FrameMargin} : findSubject(cloud, inside, scene);
    const std::vector<size_t>& subject = found.splats;
    const Vec3 up = estimatedUp.confidence >= SplatUpConfident ? estimatedUp.up : DefaultUp;
    const CameraAxes camera =
        threeQuarterView(cloud, subject, up, enclosed ? EnclosedElevationDegrees : ElevationDegrees);

    double minHalf = 0.0;
    if (found.inItsSurroundings) {
        const auto [lo, hi] = trimmedRange(along(cloud, inside, camera.right), SceneContextPercentile);
        minHalf = SceneContextShare * (hi - lo) / 2;
    }
    const Framing frame = frameSubject(cloud, subject, found.margin, minHalf, camera, TanHalfFieldOfView);
    const Cuts cuts =
        enclosed ? roomCuts(cloud, subject, up, frame) : openAirCuts(cloud, inside, subject, up, camera, frame);
    return {up, camera, frame, cuts, enclosed, subject.size()};
}

// Paints the splats as `view` sees them, composited front to back, into a picture of thumbSize.
// The output size honours thumbSize at request time - nothing is precomputed at build time.
void paintView(const SplatPreviewInput& cloud,
               const std::vector<size_t>& splats,
               const SplatView& view,
               int thumbSize,
               const fs::path& outImagePath,
               uint8_t** outBuffer,
               int* outBufferSize,
               SplatThumbStats* stats) {
    const CameraAxes& camera = view.camera;
    const Framing& frame = view.frame;
    const Cuts& cuts = view.cuts;
    const double tanHalf = TanHalfFieldOfView;
    const double focal = (thumbSize / 2.0) / tanHalf;
    const double maxRadius = std::max(MaxFootprintShare * thumbSize, MaxFootprintFloorPixels);

    // Into the picture
    SplatThumbStats counts;
    counts.splats = splats.size();
    counts.framed = view.framed;
    counts.enclosed = view.enclosed;
    const double size = thumbSize;
    std::vector<ProjectedSplat> projected;
    projected.reserve(splats.size());
    for (const size_t i : splats) {
        const Vec3 p = positionOf(cloud.positions, i);
        const double x = dot(p, camera.right) - frame.cx;
        const double y = dot(p, camera.up) - frame.cy;
        const double z = dot(p, camera.forward) - frame.cz + frame.distance;
        const double height = dot(p, view.up);
        if (z < cuts.nearZ || z > cuts.farZ || height > cuts.top || height < cuts.floor) {
            ++counts.cutAway;
            continue;
        }

        ProjectedSplat splat{};
        if (!projectSplat(cloud, i, camera, x, y, z, focal, tanHalf, splat))
            continue;
        splat.depth = static_cast<float>(z);
        splat.x = static_cast<float>(size / 2 + focal * x / z);
        splat.y = static_cast<float>(size / 2 - focal * y / z);
        if (splat.x + splat.radiusX < 0 || splat.x - splat.radiusX > size || splat.y + splat.radiusY < 0 ||
            splat.y - splat.radiusY > size)
            continue;
        if (std::max(splat.radiusX, splat.radiusY) > maxRadius) {
            ++counts.culledOversized;
            continue;
        }

        splat.opacity = static_cast<float>(std::min(1.0, opacityOf(cloud, i)));
        // Out to where it is too faint to change a pixel, or to 3 sigma if that is sooner
        splat.minPower = static_cast<float>(
            std::max(-0.5 * CutSigmas * CutSigmas, std::log(MinPixelAlpha / static_cast<double>(splat.opacity))));
        // A colour that is not a number has no value: it is black
        const auto channel = [&](int k) {
            const float v = cloud.colors.size() >= (i + 1) * 3 ? cloud.colors[i * 3 + k] : 0.5f;
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        };
        splat.r = channel(0);
        splat.g = channel(1);
        splat.bl = channel(2);
        projected.push_back(splat);
    }
    counts.drawn = projected.size();

    // Near to far, so each splat is composited behind what is in front of it
    std::sort(projected.begin(), projected.end(),
              [](const ProjectedSplat& a, const ProjectedSplat& b) { return a.depth < b.depth; });

    // The splats of each tile, nearest first: counted, then listed
    const int tilesX = (thumbSize + TilePixels - 1) / TilePixels;
    const size_t tiles = static_cast<size_t>(tilesX) * static_cast<size_t>(tilesX);
    const auto tileRange = [&](const ProjectedSplat& splat, int& x0, int& x1, int& y0, int& y1) {
        x0 = std::clamp(static_cast<int>(std::floor(splat.x - splat.radiusX)) / TilePixels, 0, tilesX - 1);
        x1 = std::clamp(static_cast<int>(std::ceil(splat.x + splat.radiusX)) / TilePixels, 0, tilesX - 1);
        y0 = std::clamp(static_cast<int>(std::floor(splat.y - splat.radiusY)) / TilePixels, 0, tilesX - 1);
        y1 = std::clamp(static_cast<int>(std::ceil(splat.y + splat.radiusY)) / TilePixels, 0, tilesX - 1);
    };
    std::vector<uint32_t> offsets(tiles + 1, 0);
    for (const ProjectedSplat& splat : projected) {
        int x0, x1, y0, y1;
        tileRange(splat, x0, x1, y0, y1);
        for (int ty = y0; ty <= y1; ++ty)
            for (int tx = x0; tx <= x1; ++tx)
                ++offsets[static_cast<size_t>(ty) * tilesX + tx + 1];
    }
    for (size_t t = 0; t < tiles; ++t)
        offsets[t + 1] += offsets[t];
    std::vector<uint32_t> lists(offsets[tiles]);
    {
        std::vector<uint32_t> next(offsets.begin(), offsets.end() - 1);
        for (size_t s = 0; s < projected.size(); ++s) {
            int x0, x1, y0, y1;
            tileRange(projected[s], x0, x1, y0, y1);
            for (int ty = y0; ty <= y1; ++ty)
                for (int tx = x0; tx <= x1; ++tx)
                    lists[next[static_cast<size_t>(ty) * tilesX + tx]++] = static_cast<uint32_t>(s);
        }
    }

    RasterImage image(thumbSize, thumbSize, 3, true);
    paintTiles(projected, offsets, lists, tilesX, thumbSize, image);
    // The buffers as large as the picture: the tile offsets and those of the painting threads
    counts.pixelBufferBytes = offsets.size() * sizeof(uint32_t) +
                              PaintThreads * TilePixels * TilePixels * 4 * sizeof(float);
    if (stats != nullptr)
        *stats = counts;

    if (std::none_of(image.alpha.begin(), image.alpha.end(), [](uint8_t a) { return a != 0; }))
        throw AppException("No splats projected into the thumbnail frame");

    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

// The chunks of a RAD the view is chosen on, whatever the size of the picture. The cut of the tree
// they hold has more splats where the scene has more detail (the cut of fewer chunks is about as
// dense everywhere, and the subject does not stand out of it).
constexpr int ViewChunks = 8;
// The chunks a thumbnail of `thumbSize` is painted from: as many splats as SplatsPerPixel times its
// pixels (most of the scene is not the subject the picture is fitted to), from the ViewChunks to
// MaxThumbChunks (about 1 million splats, 60 MiB decoded)
constexpr double SplatsPerPixel = 2.0;
constexpr int MaxThumbChunks = 16;

int thumbChunks(int thumbSize) {
    const double wanted = SplatsPerPixel * thumbSize * thumbSize / static_cast<double>(RadChunkSplats);
    return std::clamp(static_cast<int>(std::ceil(wanted)), ViewChunks, MaxThumbChunks);
}

// The up vector of the model.rad of a build when the bounds.json beside it says the splat is
// georeferenced: its frame is East/North/Up, whatever the splats look like, so the scene stands on +Z
// without an estimate. A bounds.json that is missing or unreadable is no claim.
std::optional<SplatUp> georeferencedUpOfRad(const fs::path& radPath) {
    try {
        std::ifstream in(radPath.parent_path() / GsplatBoundsFileName);
        json bounds;
        if (in.good() && (in >> bounds) && bounds.is_object() &&
            bounds.value("upSource", "") == splatUpSourceName(SplatUpSource::Georef))
            return estimateSplatUp(RadCoarseSplats(), /*georeferenced=*/true);
    } catch (const std::exception&) {
        // not a claim: the estimate stands
    }
    return std::nullopt;
}

}  // namespace

// Render an on-demand preview of a Gaussian Splat (.spz). Colours come from the spherical-harmonics
// DC term, opacity from the stored logit and scales from their logarithm; all are converted to
// display space before handing off to the shared renderer.
void generateSplatThumb(const fs::path& spzPath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize,
                        SplatThumbStats* stats) {
    assureSplatThumbSize(thumbSize);

    spz::GaussianCloud cloud;
    try {
        spz::UnpackOptions unpack;
        cloud = spz::loadSpz(io::readAllBytes(spzPath), unpack); // not the file name overload (mio.h, "Path encoding")
    } catch (const std::exception& e) {
        throw AppException(std::string("Cannot decode .spz for thumbnail: ") + e.what());
    }

    const size_t n = static_cast<size_t>(std::max(cloud.numPoints, 0));
    if (n == 0 || cloud.positions.size() < n * 3)
        throw AppException("No splats to render in " + io::toUtf8(spzPath));

    // Convert SPZ-native attributes to display space: SH DC term -> RGB, logit -> opacity, log -> scale.
    std::vector<float> colors(n * 3, 0.5f);
    std::vector<float> opacities(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const float logit = (i < cloud.alphas.size()) ? cloud.alphas[i] : 0.0f;
        opacities[i] = splat_math::sigmoid(logit);
        if (cloud.colors.size() >= (i + 1) * 3) {
            for (int c = 0; c < 3; ++c)
                colors[i * 3 + c] = splat_math::dcToColor(cloud.colors[i * 3 + c]);
        }
    }
    std::vector<float> scales;
    if (cloud.scales.size() >= n * 3) {
        scales.resize(n * 3);
        std::transform(cloud.scales.begin(), cloud.scales.begin() + static_cast<std::ptrdiff_t>(n * 3), scales.begin(),
                       [](float logScale) { return std::exp(logScale); });
    }

    const SplatPreviewInput input{cloud.positions, colors, opacities, scales, cloud.rotations, n, io::toUtf8(spzPath)};
    const std::vector<size_t> splats = participating(input);
    const SplatView view = chooseView(input, splats, estimateSplatUp(cloud.positions, opacities));
    paintView(input, splats, view, thumbSize, outImagePath, outBuffer, outBufferSize, stats);
}

// Render an on-demand preview from a Spark RAD level-of-detail file. The coarsest chunks are
// decoded, down to the cut of the tree they hold (a low-density view of the whole scene, each region
// once), as many as the size of the picture calls for, so this stays cheap enough to run per
// request - the same on-the-fly model used for COPC point clouds. RAD colours are already display
// RGB, opacities already activated and scales linear, so no conversion is required. The up vector is
// estimated from the chunks bounds.json has it from, so the thumbnail stands the scene as the viewer
// does.
void generateSplatThumbFromRad(const fs::path& radPath,
                               int thumbSize,
                               const fs::path& outImagePath,
                               uint8_t** outBuffer,
                               int* outBufferSize,
                               SplatThumbStats* stats) {
    assureSplatThumbSize(thumbSize);

    // The scene stands on the up vector of bounds.json (estimated from the same chunks) and is seen the
    // same way at any size; the picture is painted from as many chunks as its size calls for
    const auto read = [&](int chunks) {
        try {
            return readRadCoarseSplats(radPath, chunks, /*onlyTheCut=*/true);
        } catch (const std::exception& e) {
            throw AppException(std::string("Cannot decode .rad for thumbnail: ") + e.what());
        }
    };
    const auto inputOf = [&](const RadCoarseSplats& splats) {
        return SplatPreviewInput{splats.positions, splats.colors, splats.opacities, splats.scales, splats.rotations,
                                 splats.count, io::toUtf8(radPath)};
    };

    const std::optional<SplatUp> georeferencedUp = georeferencedUpOfRad(radPath);
    const SplatUp up = georeferencedUp ? *georeferencedUp : estimateSplatUp(read(RadPreviewChunks));
    RadCoarseSplats splats = read(ViewChunks);
    std::vector<size_t> taking = participating(inputOf(splats));
    const SplatView view = chooseView(inputOf(splats), taking, up);

    const int chunks = thumbChunks(thumbSize);
    if (chunks > ViewChunks) {
        splats = RadCoarseSplats(); // one cut in memory at a time
        splats = read(chunks);
        taking = participating(inputOf(splats));
    }
    paintView(inputOf(splats), taking, view, thumbSize, outImagePath, outBuffer, outBufferSize, stats);
}

}  // namespace ddb
