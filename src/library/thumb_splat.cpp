/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "exceptions.h"
#include "mio.h"
#include "rad.h"
#include "thumb_internal.h"

// SPZ (vendored, MIT) - decode .spz to render an on-demand splat thumbnail.
#include "load-spz.h"
#include "splat-types.h"
#include "gsplat.h"

namespace ddb {

namespace {

// A cloud of splats in display space: `colors` holds RGB in [0,1] and `opacities` the activated
// opacity (NOT logits)
struct SplatPreviewInput {
    const std::vector<float>& positions;
    const std::vector<float>& colors;
    const std::vector<float>& opacities;
    size_t count;
    std::string sourceLabel;
};

// How the scene lands in the thumbnail: the plane perpendicular to its flattest axis (so aerial
// scenes naturally render top-down), scaled and centred in the tile
struct PreviewFrame {
    double mn[3];
    int depthAxis;
    int uAxis;
    int vAxis;
    double scale;
    double offX;
    double offY;
};

PreviewFrame previewFrame(const std::vector<float>& positions, size_t n, int tileSize) {
    PreviewFrame frame{};
    double* mn = frame.mn;
    mn[0] = mn[1] = mn[2] = std::numeric_limits<double>::max();
    double mx[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            const double v = static_cast<double>(positions[i * 3 + k]);
            mn[k] = std::min(mn[k], v);
            mx[k] = std::max(mx[k], v);
        }
    }

    const double extent[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};

    // Depth axis = flattest dimension; the remaining two form the image plane.
    int depthAxis = 0;
    if (extent[1] < extent[depthAxis]) depthAxis = 1;
    if (extent[2] < extent[depthAxis]) depthAxis = 2;
    frame.depthAxis = depthAxis;
    frame.uAxis = (depthAxis == 0) ? 1 : 0;
    frame.vAxis = (depthAxis == 2) ? 1 : 2;

    const double uExtent = extent[frame.uAxis];
    const double vExtent = extent[frame.vAxis];
    if (uExtent <= 0.0 && vExtent <= 0.0)
        throw GDALException("Gaussian Splat has zero planar extent; cannot render thumbnail");

    // Fit the (u, v) extent into the tile preserving aspect ratio, with a 1px margin.
    const double drawable = static_cast<double>(tileSize - 1);
    const double scaleU = uExtent > 0.0 ? drawable / uExtent : 0.0;
    const double scaleV = vExtent > 0.0 ? drawable / vExtent : 0.0;
    double scale = std::min(scaleU > 0.0 ? scaleU : scaleV, scaleV > 0.0 ? scaleV : scaleU);
    if (scale <= 0.0) scale = 1.0;
    frame.scale = scale;
    frame.offX = (tileSize - uExtent * scale) / 2.0;
    frame.offY = (tileSize - vExtent * scale) / 2.0;
    return frame;
}

// Shared orthographic rasteriser for Gaussian Splat previews. The scene is projected onto the
// plane of its frame and depth-sorted with a simple z-buffer. The output size honours
// thumbSize at request time - nothing is precomputed at build time.
void rasterizeSplatPreview(const SplatPreviewInput& cloud,
                           int thumbSize,
                           const fs::path& outImagePath,
                           uint8_t** outBuffer,
                           int* outBufferSize) {
    const std::vector<float>& positions = cloud.positions;
    const std::vector<float>& colors = cloud.colors;
    const std::vector<float>& opacities = cloud.opacities;
    const size_t n = cloud.count;
    if (n == 0 || positions.size() < n * 3)
        throw InvalidArgsException("No splats to render in " + cloud.sourceLabel);

    const PreviewFrame frame = previewFrame(positions, n, thumbSize);
    const double* mn = frame.mn;
    const int depthAxis = frame.depthAxis;
    const int uAxis = frame.uAxis;
    const int vAxis = frame.vAxis;

    const int tileSize = thumbSize;
    const size_t wSize = static_cast<size_t>(tileSize) * static_cast<size_t>(tileSize);

    // Band-sequential RGB (3 planes) + a separate alpha plane, as in RasterImage
    std::vector<uint8_t> buffer(wSize * 3, 0);
    std::vector<uint8_t> alphaBuffer(wSize, 0);
    std::vector<float> zBuffer(wSize, std::numeric_limits<float>::lowest());

    const auto toByte = [](double v) -> uint8_t {
        return static_cast<uint8_t>(std::lround(std::min(std::max(v, 0.0), 1.0) * 255.0));
    };

    // Point radius scales with thumbnail size so larger previews keep visual density.
    // At 256 px the radius is 1; at 1024 px it is 4. Clamped to [1, 8].
    const int radius = std::max(1, std::min(8, static_cast<int>(std::lround(static_cast<double>(tileSize) / 256.0))));
    const int radiusSq = radius * radius;

    size_t rendered = 0;
    for (size_t i = 0; i < n; ++i) {
        // Opacities are already activated; skip near-transparent splats.
        const double opacity = (i < opacities.size()) ? static_cast<double>(opacities[i]) : 1.0;
        if (opacity < 0.02)
            continue;

        const double u = static_cast<double>(positions[i * 3 + uAxis]);
        const double v = static_cast<double>(positions[i * 3 + vAxis]);
        const double d = static_cast<double>(positions[i * 3 + depthAxis]);

        int cx = static_cast<int>((u - mn[uAxis]) * frame.scale + frame.offX);
        int cy = static_cast<int>((v - mn[vAxis]) * frame.scale + frame.offY);
        // Flip vertically so "up" in world space points up in the image.
        cy = tileSize - 1 - cy;

        double rgb[3] = {0.5, 0.5, 0.5};
        if (colors.size() >= (i + 1) * 3) {
            for (int c = 0; c < 3; ++c)
                rgb[c] = static_cast<double>(colors[i * 3 + c]);
        }
        const uint8_t br = toByte(rgb[0]);
        const uint8_t bg = toByte(rgb[1]);
        const uint8_t bb = toByte(rgb[2]);
        const float dz = static_cast<float>(d);

        // Draw a filled circle, respecting the z-buffer.
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy > radiusSq)
                    continue;
                const int px = cx + dx;
                const int py = cy + dy;
                if (px < 0 || px >= tileSize || py < 0 || py >= tileSize)
                    continue;
                const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(tileSize) +
                                   static_cast<size_t>(px);
                if (dz <= zBuffer[idx])
                    continue;
                zBuffer[idx] = dz;
                buffer[0 * wSize + idx] = br;
                buffer[1 * wSize + idx] = bg;
                buffer[2 * wSize + idx] = bb;
                alphaBuffer[idx] = 255;
                ++rendered;
            }
        }
    }

    if (rendered == 0)
        throw GDALException("No splats projected into the thumbnail frame");

    RasterImage image(tileSize, tileSize, 3, false);
    image.color = std::move(buffer);
    image.alpha = std::move(alphaBuffer);
    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

}  // namespace

// Render an on-demand orthographic preview of a Gaussian Splat (.spz). Colours come from the
// spherical-harmonics DC term and opacity from the stored logit; both are converted to display
// space before handing off to the shared rasteriser.
void generateSplatThumb(const fs::path& spzPath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    spz::GaussianCloud cloud;
    try {
        spz::UnpackOptions unpack;
        cloud = spz::loadSpz(io::readAllBytes(spzPath), unpack); // not the file name overload (gsplat.cpp)
    } catch (const std::exception& e) {
        throw AppException(std::string("Cannot decode .spz for thumbnail: ") + e.what());
    }

    const size_t n = static_cast<size_t>(std::max(cloud.numPoints, 0));
    if (n == 0 || cloud.positions.size() < n * 3)
        throw InvalidArgsException("No splats to render in " + io::toUtf8(spzPath));

    // Convert SPZ-native attributes to display space: SH DC term -> RGB, logit -> opacity.
    std::vector<float> colors(n * 3, 0.5f);
    std::vector<float> opacities(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const float logit = (i < cloud.alphas.size()) ? cloud.alphas[i] : 0.0f;
        opacities[i] = 1.0f / (1.0f + std::exp(-logit));
        if (cloud.colors.size() >= (i + 1) * 3) {
            for (int c = 0; c < 3; ++c)
                colors[i * 3 + c] =
                    static_cast<float>(0.5 + SH_C0 * static_cast<double>(cloud.colors[i * 3 + c]));
        }
    }

    rasterizeSplatPreview({cloud.positions, colors, opacities, n, io::toUtf8(spzPath)}, thumbSize,
                          outImagePath, outBuffer, outBufferSize);
}

// Render an on-demand preview from a Spark RAD level-of-detail file. The two coarsest chunks are
// decoded (a low-density view of the whole scene), so this stays cheap enough to run per request -
// the same on-the-fly model used for COPC point clouds. RAD colours are already display RGB and
// opacities already activated, so no conversion is required.
void generateSplatThumbFromRad(const fs::path& radPath,
                               int thumbSize,
                               const fs::path& outImagePath,
                               uint8_t** outBuffer,
                               int* outBufferSize) {
    RadCoarseSplats splats;
    try {
        splats = readRadCoarseSplats(radPath, /*maxChunks=*/2);
    } catch (const std::exception& e) {
        throw AppException(std::string("Cannot decode .rad for thumbnail: ") + e.what());
    }

    rasterizeSplatPreview({splats.positions, splats.colors, splats.opacities, splats.count,
                           io::toUtf8(radPath)},
                          thumbSize, outImagePath, outBuffer, outBufferSize);
}

}  // namespace ddb
