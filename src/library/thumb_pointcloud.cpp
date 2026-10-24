/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <pdal/io/CopcReader.hpp>
#include <sstream>

#include "coordstransformer.h"
#include "copc_utils.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "pointcloud.h"
#include "point_rasterizer.h"
#include "thumb_internal.h"
#include "tiler.h"

namespace ddb {

namespace {

// A point-cloud thumbnail reads about this many points per output pixel: enough for
// the 2-px discs to cover the footprint, a fraction of the cloud
constexpr uint64_t PointsPerThumbnailPixel = 2;

// The area a cloud covers: in EPSG:3857 when the cloud has a spatial system and its polygon
// bounds are usable, otherwise in the units of its own bounds
struct CloudFootprint {
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    double length = 0;  // the shorter side
    bool georeferenced = false;
};

CloudFootprint cloudFootprint(PointCloudInfo& pcInfo) {
    CloudFootprint f;
    f.georeferenced = !pcInfo.wktProjection.empty() && !pcInfo.polyBounds.empty();

    if (f.georeferenced) {
        f.minX = pcInfo.polyBounds.getPoint(0).x;
        f.maxX = pcInfo.polyBounds.getPoint(2).x;
        f.maxY = pcInfo.polyBounds.getPoint(2).y;
        f.minY = pcInfo.polyBounds.getPoint(0).y;
    } else {
        f.minX = pcInfo.bounds[0];
        f.minY = pcInfo.bounds[1];
        f.maxX = pcInfo.bounds[3];
        f.maxY = pcInfo.bounds[4];
    }

    // The length decides the zoom level when the octree cannot be planned
    f.length = std::min(std::abs(f.maxX - f.minX), std::abs(f.maxY - f.minY));

    if (f.length == 0) {
        // Fallback to raw bounds if transformed bounds are invalid
        f.minX = pcInfo.bounds[0];
        f.maxX = pcInfo.bounds[3];
        f.maxY = pcInfo.bounds[4];
        f.minY = pcInfo.bounds[1];

        f.length = std::min(std::abs(f.maxX - f.minX), std::abs(f.maxY - f.minY));

        if (f.length == 0) {
            throw GDALException("Cannot calculate length: point cloud has zero extent");
        }

        f.georeferenced = false;
    }
    return f;
}

bool hasColorDimensions(const PointCloudInfo& pcInfo) {
    const auto has = [&](const char* name) {
        return std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), name) !=
               pcInfo.dimensions.end();
    };
    return has("Red") && has("Green") && has("Blue");
}

// PDAL's remote reads (arbiter) look for the CA bundle in the environment
void exportCaBundleForRemoteReads() {
#ifdef _WIN32
    const fs::path caBundlePath = io::getDataPath("curl-ca-bundle.crt");
    if (!caBundlePath.empty()) {
        LOGD << "ARBITRER CA Bundle: " << io::toUtf8(caBundlePath);
        std::stringstream ss;
        ss << "ARBITER_CA_INFO=" << io::toUtf8(caBundlePath);
        if (_putenv(ss.str().c_str()) != 0) {
            LOGD << "Cannot set ARBITER_CA_INFO";
        }
    }
#endif
}

// The resolution to read the octree at: down to the deepest level whose points fit the budget
// (about PointsPerThumbnailPixel per output pixel), not every point finer than a pixel
double thumbnailResolution(const fs::path& copcPath, uint64_t budget, const GlobalMercator& mercator,
                           double length) {
    try {
        const auto hierarchy = readCopcHierarchy(io::toUtf8(copcPath));
        const int depth = planCopcDepth(hierarchy.pointsPerDepth, budget);
        const double resolution = copcResolutionForDepth(hierarchy.spacing, depth);
        LOGD << "Point cloud thumbnail reads COPC depths 0-" << depth << " (resolution " << resolution << ")";
        return resolution;
    } catch (const AppException& e) {
        // Pixel-size heuristic: reads every level finer than a pixel
        LOGD << "Cannot plan COPC read (" << e.what() << "), using the pixel size";
        const auto tz = mercator.zoomForLength(length);
        return tz < 0 ? 1 : mercator.resolution(tz);
    }
}

// Fits the footprint in the square thumbnail, centred
PixelMapping fitFootprint(const CloudFootprint& footprint, int tileSize) {
    const double width = footprint.maxX - footprint.minX;
    const double height = footprint.maxY - footprint.minY;
    const double tileScaleW = tileSize / width;
    const double tileScaleH = tileSize / height;

    PixelMapping mapping;
    mapping.originX = footprint.minX;
    mapping.originY = footprint.minY;
    if (tileScaleW > tileScaleH) {
        // Taller than wider
        mapping.scaleX = mapping.scaleY = tileScaleH;
        mapping.offsetX = (tileSize - width * tileScaleH) / 2;
    } else {
        // Wider than taller
        mapping.scaleX = mapping.scaleY = tileScaleW;
        mapping.offsetY = (tileSize - height * tileScaleW) / 2;
    }
    mapping.width = mapping.height = tileSize;
    return mapping;
}

// Rasterizes every point the reader delivers (coloured by the cloud, or grey from the elevation
// when it has no colours); returns how many were read
size_t rasterizePoints(pdal::CopcReader& reader, const PointCloudInfo& pcInfo, bool georeferenced,
                       bool hasColors, PointRasterizer& rasterizer) {
    std::unique_ptr<CoordsTransformer> toMercator;
    if (georeferenced)
        toMercator = std::make_unique<CoordsTransformer>(pcInfo.wktProjection, 3857);

    const double minZ = pcInfo.bounds[2];
    const double zRange = pcInfo.bounds[5] - minZ;

    size_t pointsRead = 0;
    try {
        forEachPointBatch(reader, [&](PointBatch& points) {
            pointsRead += points.size();
            if (!hasColors) {
                // Grey from elevation: lower is darker
                for (size_t i = 0; i < points.size(); i++) {
                    const uint16_t intensity = zRange > 0
                        ? static_cast<uint8_t>((points.z[i] - minZ) / zRange * 255)
                        : 128;
                    points.red[i] = points.green[i] = points.blue[i] = intensity;
                }
            }
            if (toMercator)
                toMercator->transform(points.size(), points.x.data(), points.y.data());
            rasterizer.add(points);
        });
    } catch (const pdal::pdal_error& e) {
        throw PDALException("PDAL error: " + std::string(e.what()));
    }
    return pointsRead;
}

}  // namespace

void generatePointCloudThumb(const fs::path& copcPath,
                             int thumbSize,
                             const fs::path& outImagePath,
                             uint8_t** outBuffer,
                             int* outBufferSize,
                             uint64_t pointBudget) {
    PointCloudInfo pcInfo;

    // Load COPC information
    int span;
    if (!getCopcInfo(io::toUtf8(copcPath), pcInfo, 3857, &span)) {
        throw InvalidArgsException("Cannot get COPC info for " + io::toUtf8(copcPath));
    }

    // Validate bounds array has required elements (minX, minY, minZ, maxX, maxY, maxZ)
    if (pcInfo.bounds.size() < 6) {
        throw InvalidArgsException("COPC bounds array does not contain at least 6 elements (minX, minY, minZ, maxX, maxY, maxZ required)");
    }

    const auto tileSize = thumbSize;
    GlobalMercator mercator(tileSize);

    const CloudFootprint footprint = cloudFootprint(pcInfo);
    const bool hasColors = hasColorDimensions(pcInfo);

    exportCaBundleForRemoteReads();

    const uint64_t budget = pointBudget > 0
        ? pointBudget
        : PointsPerThumbnailPixel * static_cast<uint64_t>(thumbSize) * static_cast<uint64_t>(thumbSize);
    const double resolution = thumbnailResolution(copcPath, budget, mercator, footprint.length);

    const auto copcThreads = copcReadThreads(io::toUtf8(copcPath));  // until the reader has run
    pdal::Options copcOpts = copcReaderOptions(io::toUtf8(copcPath), copcThreads);
    copcOpts.add("resolution", resolution);

    pdal::CopcReader copcReader;
    copcReader.setOptions(copcOpts);

    PointRasterizer rasterizer(tileSize, fitFootprint(footprint, tileSize));
    const size_t pointsRead =
        rasterizePoints(copcReader, pcInfo, footprint.georeferenced, hasColors, rasterizer);

    if (pointsRead == 0)
        throw GDALException("No points fetched from cloud, check zoom level");

    thumb::write(rasterizer.toImage(), outImagePath, outBuffer, outBufferSize);
}

}  // namespace ddb
