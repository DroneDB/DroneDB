/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "exceptions.h"
#include "mio.h"
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

// A splat more transparent than this takes no part, in the frame or in the picture
constexpr double MinSplatOpacity = 0.02;
// A pixel that a splat changes by less than one level of an 8-bit channel is left as it is
constexpr double MinPixelAlpha = 1.0 / 255.0;
// The ellipse of a splat is drawn out to this many standard deviations
constexpr double CutSigmas = 3.0;
// The low-pass added to the covariance of a splat on the screen: nothing is smaller than a pixel
constexpr double LowPassPixels = 0.3;

// The largest thumbnail rendered from a splat: the tile buffers take 20 bytes a pixel (16 for the
// colour and coverage being composited, 4 for the picture), 320 MiB at this size
constexpr int MaxSplatThumbSize = 4096;

void assureSplatThumbSize(int thumbSize) {
    if (thumbSize <= 0 || thumbSize > MaxSplatThumbSize)
        throw InvalidArgsException("thumbSize must be between 1 and " + std::to_string(MaxSplatThumbSize) +
                                   " for a Gaussian Splat, got " + std::to_string(thumbSize));
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

// Vertical field of view of the thumbnail; a long lens: the splats keep their proportions
constexpr double FieldOfViewDegrees = 40.0;
// How the three-quarter view stands: above the ground by the elevation, and turned from the
// axis along which the scene is narrowest by the azimuth, towards the one along which it is longest
constexpr double ElevationDegrees = 30.0;
constexpr double AzimuthDegrees = 35.0;
constexpr double Pi = 3.14159265358979323846;
// The share of the splats the frame is fitted to, at each end of each axis: floaters and a
// far background do not make the scene small
constexpr double FramePercentile = 0.02;
// Room left around the scene
constexpr double FrameMargin = 1.08;
// Splats nearer the camera than this share of its distance to the scene are not drawn
constexpr double NearShare = 0.1;

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

// The three-quarter view of a scene that stands on its up vector: the picture is wide along the
// longest direction of the ground, and the camera looks down on it
CameraAxes threeQuarterView(const SplatPreviewInput& cloud, const std::vector<size_t>& splats, const Vec3& up) {
    const Vec3 e1 = anyPerpendicular(up);
    const Vec3 e2 = cross(up, e1);

    // The spread of the splats over the ground: the axes of its 2x2 covariance
    Vec3 mean = {0, 0, 0};
    for (const size_t i : splats) {
        const Vec3 p = positionOf(cloud.positions, i);
        mean[0] += dot(p, e1);
        mean[1] += dot(p, e2);
    }
    mean = scaled(mean, 1.0 / static_cast<double>(splats.size()));
    double c11 = 0, c12 = 0, c22 = 0;
    for (const size_t i : splats) {
        const Vec3 p = positionOf(cloud.positions, i);
        const double a = dot(p, e1) - mean[0], b = dot(p, e2) - mean[1];
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

    const double elevation = ElevationDegrees * Pi / 180.0;
    const double azimuth = AzimuthDegrees * Pi / 180.0;
    const Vec3 ground = plus(scaled(minor, std::cos(azimuth)), scaled(major, std::sin(azimuth)));
    const Vec3 toCamera = plus(scaled(ground, std::cos(elevation)), scaled(up, std::sin(elevation)));
    return cameraLooking(scaled(toCamera, -1.0), up);
}

// The view a scene gets when it has no up of its own: straight along its flattest axis, the picture
// being the plane of the other two (so an aerial scene is seen from above, as a map)
CameraAxes alongFlattestAxis(const SplatPreviewInput& cloud, const std::vector<size_t>& splats) {
    Vec3 mn = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
               std::numeric_limits<double>::max()};
    Vec3 mx = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
               std::numeric_limits<double>::lowest()};
    for (const size_t i : splats) {
        const Vec3 p = positionOf(cloud.positions, i);
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], p[k]);
            mx[k] = std::max(mx[k], p[k]);
        }
    }
    int depthAxis = 0;
    if (mx[1] - mn[1] < mx[depthAxis] - mn[depthAxis]) depthAxis = 1;
    if (mx[2] - mn[2] < mx[depthAxis] - mn[depthAxis]) depthAxis = 2;
    const int uAxis = (depthAxis == 0) ? 1 : 0;
    const int vAxis = (depthAxis == 2) ? 1 : 2;

    Vec3 right = {0, 0, 0}, up = {0, 0, 0};
    right[uAxis] = 1.0;
    up[vAxis] = 1.0;
    // The camera is on the side of the depth axis that makes the picture a view and not a mirror
    return cameraLooking(scaled(cross(right, up), -1.0), up);
}

// ---------------------------------------------------------------------------
// The projection
// ---------------------------------------------------------------------------

// What the compositing needs of a splat once it is in the picture
struct ProjectedSplat {
    float depth;
    float x, y;       // centre, in pixels
    float a, b, c;    // inverse of the 2D covariance (the conic): a dx^2 + 2 b dx dy + c dy^2
    float radiusX, radiusY; // half the extent of its 3 sigma ellipse
    float opacity;
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

// Where the camera stands: it looks at the middle of the scene, and is far enough for the part of
// the scene the picture is fitted to (all but the outermost splats) to be in it
struct Framing {
    double cx, cy, cz; // the middle of the scene, along the axes of the camera
    double distance;   // from the camera to the middle of the scene
};

// The outermost `FramePercentile` of the values at each end are left out
std::pair<double, double> trimmedRange(std::vector<double> values) {
    const size_t trim = static_cast<size_t>(FramePercentile * static_cast<double>(values.size() - 1));
    const size_t hi = values.size() - 1 - trim;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(trim), values.end());
    const double lo = values[trim];
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(hi), values.end());
    return {lo, values[hi]};
}

Framing frameScene(const SplatPreviewInput& cloud, const std::vector<size_t>& splats, const CameraAxes& camera,
                   double tanHalf) {
    std::vector<double> xs, ys, zs;
    xs.reserve(splats.size());
    ys.reserve(splats.size());
    zs.reserve(splats.size());
    for (const size_t i : splats) {
        const Vec3 p = positionOf(cloud.positions, i);
        xs.push_back(dot(p, camera.right));
        ys.push_back(dot(p, camera.up));
        zs.push_back(dot(p, camera.forward));
    }
    const auto [xLo, xHi] = trimmedRange(xs);
    const auto [yLo, yHi] = trimmedRange(ys);
    const auto [zLo, zHi] = trimmedRange(zs);

    Framing frame{(xLo + xHi) / 2, (yLo + yHi) / 2, (zLo + zHi) / 2, 0.0};
    double half = std::max(xHi - xLo, yHi - yLo) / 2;
    if (!(half > 0.0)) {
        // The centres are on one line of sight: what the picture can show is the splats themselves
        for (const size_t i : splats)
            for (int k = 0; k < 3; ++k) {
                const double v = cloud.scales.size() >= (i + 1) * 3 ? cloud.scales[i * 3 + k] : 0.0;
                if (std::isfinite(v)) half = std::max(half, CutSigmas * v);
            }
        if (!(half > 0.0))
            throw AppException("Gaussian Splat has zero planar extent; cannot render thumbnail");
    }
    frame.distance = half * FrameMargin / tanHalf + (zHi - zLo) / 2;
    return frame;
}

// ---------------------------------------------------------------------------
// The picture
// ---------------------------------------------------------------------------

// Renders the splats as a perspective view and composites their Gaussian footprints back to front.
// The output size honours thumbSize at request time - nothing is precomputed at build time.
//
// Which way the camera looks depends on the scene: one that stands on an up vector (estimateSplatUp
// is sure of it) is seen in three-quarter view from above its ground, upright; any other from
// straight above its flattest axis, as a map.
void renderSplatPreview(const SplatPreviewInput& cloud,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    const size_t n = cloud.count;
    if (n == 0 || cloud.positions.size() < n * 3)
        throw AppException("No splats to render in " + cloud.sourceLabel);

    // The splats that take part: a position that is a number, and some opacity (a NaN has none)
    std::vector<size_t> splats;
    bool anyFinite = false;
    for (size_t i = 0; i < n; ++i) {
        if (!hasFinitePosition(cloud.positions, i))
            continue;
        anyFinite = true;
        const double opacity = (i < cloud.opacities.size()) ? static_cast<double>(cloud.opacities[i]) : 1.0;
        if (opacity >= MinSplatOpacity)
            splats.push_back(i);
    }
    if (!anyFinite)
        throw AppException("Gaussian Splat has no finite position; cannot render thumbnail");
    if (splats.empty())
        throw AppException("No splats projected into the thumbnail frame");

    const SplatUp up = estimateSplatUp(cloud.positions, cloud.opacities);
    const CameraAxes camera = up.confidence >= SplatUpConfident ? threeQuarterView(cloud, splats, up.up)
                                                                : alongFlattestAxis(cloud, splats);

    const double tanHalf = std::tan(FieldOfViewDegrees * Pi / 360.0);
    const double focal = (thumbSize / 2.0) / tanHalf;
    const Framing frame = frameScene(cloud, splats, camera, tanHalf);

    // Into the picture
    const double size = thumbSize;
    std::vector<ProjectedSplat> projected;
    projected.reserve(splats.size());
    for (const size_t i : splats) {
        const Vec3 p = positionOf(cloud.positions, i);
        const double x = dot(p, camera.right) - frame.cx;
        const double y = dot(p, camera.up) - frame.cy;
        const double z = dot(p, camera.forward) - frame.cz + frame.distance;
        if (z < NearShare * frame.distance)
            continue;

        ProjectedSplat splat{};
        if (!projectSplat(cloud, i, camera, x, y, z, focal, tanHalf, splat))
            continue;
        splat.depth = static_cast<float>(z);
        splat.x = static_cast<float>(size / 2 + focal * x / z);
        splat.y = static_cast<float>(size / 2 - focal * y / z);
        if (splat.x + splat.radiusX < 0 || splat.x - splat.radiusX > size || splat.y + splat.radiusY < 0 ||
            splat.y - splat.radiusY > size)
            continue;

        splat.opacity = static_cast<float>(std::min(1.0, static_cast<double>(i < cloud.opacities.size() ? cloud.opacities[i] : 1.0f)));
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

    // Far to near, so each splat is composited over what is behind it
    std::sort(projected.begin(), projected.end(),
              [](const ProjectedSplat& a, const ProjectedSplat& b) { return a.depth > b.depth; });

    // Premultiplied colour and coverage of each pixel
    const int tile = thumbSize;
    const size_t pixels = static_cast<size_t>(tile) * static_cast<size_t>(tile);
    std::vector<float> accum(pixels * 4, 0.0f);

    constexpr double CutPower = -0.5 * CutSigmas * CutSigmas;
    constexpr float MaxAlpha = 0.99f;
    for (const ProjectedSplat& splat : projected) {
        const int x0 = std::max(0, static_cast<int>(std::floor(splat.x - splat.radiusX)));
        const int x1 = std::min(tile - 1, static_cast<int>(std::ceil(splat.x + splat.radiusX)));
        const int y0 = std::max(0, static_cast<int>(std::floor(splat.y - splat.radiusY)));
        const int y1 = std::min(tile - 1, static_cast<int>(std::ceil(splat.y + splat.radiusY)));
        // Out to where it is too faint to change a pixel, or to 3 sigma if that is sooner
        const float minPower = static_cast<float>(
            std::max(CutPower, std::log(MinPixelAlpha / static_cast<double>(splat.opacity))));

        for (int py = y0; py <= y1; ++py) {
            const float dy = static_cast<float>(py) + 0.5f - splat.y;
            float* row = &accum[(static_cast<size_t>(py) * tile + x0) * 4];
            for (int px = x0; px <= x1; ++px, row += 4) {
                const float dx = static_cast<float>(px) + 0.5f - splat.x;
                const float power = -0.5f * (splat.a * dx * dx + 2.0f * splat.b * dx * dy + splat.c * dy * dy);
                if (power < minPower)
                    continue;
                const float alpha = std::min(MaxAlpha, splat.opacity * std::exp(power));
                const float keep = 1.0f - alpha;
                row[0] = splat.r * alpha + row[0] * keep;
                row[1] = splat.g * alpha + row[1] * keep;
                row[2] = splat.bl * alpha + row[2] * keep;
                row[3] = alpha + row[3] * keep;
            }
        }
    }

    // Band-sequential RGB (3 planes) + a separate alpha plane, as in RasterImage
    RasterImage image(tile, tile, 3, true);
    size_t painted = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const float* px = &accum[i * 4];
        if (px[3] < MinPixelAlpha)
            continue;
        for (int k = 0; k < 3; ++k)
            image.color[static_cast<size_t>(k) * pixels + i] =
                static_cast<uint8_t>(std::lround(std::clamp(px[k] / px[3], 0.0f, 1.0f) * 255.0f));
        image.alpha[i] = static_cast<uint8_t>(std::lround(std::min(px[3], 1.0f) * 255.0f));
        ++painted;
    }
    if (painted == 0)
        throw AppException("No splats projected into the thumbnail frame");

    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

}  // namespace

// Render an on-demand preview of a Gaussian Splat (.spz). Colours come from the spherical-harmonics
// DC term, opacity from the stored logit and scales from their logarithm; all are converted to
// display space before handing off to the shared renderer.
void generateSplatThumb(const fs::path& spzPath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
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

    renderSplatPreview({cloud.positions, colors, opacities, scales, cloud.rotations, n, io::toUtf8(spzPath)},
                       thumbSize, outImagePath, outBuffer, outBufferSize);
}

// Render an on-demand preview from a Spark RAD level-of-detail file. The coarsest chunks are
// decoded, down to the cut of the tree they hold (a low-density view of the whole scene, each region
// once), so this stays cheap enough to run per request - the same on-the-fly model used for COPC
// point clouds. RAD colours are already display RGB,
// opacities already activated and scales linear, so no conversion is required.
void generateSplatThumbFromRad(const fs::path& radPath,
                               int thumbSize,
                               const fs::path& outImagePath,
                               uint8_t** outBuffer,
                               int* outBufferSize) {
    assureSplatThumbSize(thumbSize);

    RadCoarseSplats splats;
    try {
        splats = readRadCoarseSplats(radPath, RadPreviewChunks, /*onlyTheCut=*/true);
    } catch (const std::exception& e) {
        throw AppException(std::string("Cannot decode .rad for thumbnail: ") + e.what());
    }

    renderSplatPreview({splats.positions, splats.colors, splats.opacities, splats.scales, splats.rotations,
                        splats.count, io::toUtf8(radPath)},
                       thumbSize, outImagePath, outBuffer, outBufferSize);
}

}  // namespace ddb
