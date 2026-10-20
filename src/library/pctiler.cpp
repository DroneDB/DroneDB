/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "pctiler.h"

#include <cstdlib>
#include <iomanip>
#include <memory>
#include <optional>
#include <vector>

#include <pdal/Options.hpp>
#include <pdal/io/CopcReader.hpp>
#include <pdal/filters/ColorinterpFilter.hpp>

#include "copc_node_cache.h"
#include "copc_utils.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "point_rasterizer.h"
#include "utils.h"

namespace ddb
{

    PointCloudTiler::PointCloudTiler(const std::string &inputPath, const std::string &outputFolder,
                       int tileSize, bool tms)
        : Tiler(inputPath, outputFolder, tileSize, tms)
    {

        // Open COPC and gather metadata
        int span;
        if (!getCopcInfo(inputPath, pcInfo, 3857, &span))
            throw InvalidArgsException("Cannot get COPC info for " + inputPath);

        if (pcInfo.wktProjection.empty())
            throw InvalidArgsException("COPC file has no WKT SRS: " + inputPath);

        oMinX = pcInfo.polyBounds.getPoint(0).x;
        oMaxX = pcInfo.polyBounds.getPoint(2).x;
        oMaxY = pcInfo.polyBounds.getPoint(2).y;
        oMinY = pcInfo.polyBounds.getPoint(0).y;

        LOGD << "Bounds (output SRS): (" << oMinX << "; " << oMinY << ") - ("
             << oMaxX << "; " << oMaxY << ")";

        // Max/min zoom level
        tMinZ = mercator.zoomForLength(std::min(oMaxX - oMinX, oMaxY - oMinY));
        tMaxZ = tMinZ + static_cast<int>(std::round(std::log(static_cast<double>(span) / 4.0) / std::log(2)));

        LOGD << "MinZ: " << tMinZ;
        LOGD << "MaxZ: " << tMaxZ;

        hasColors = std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), "Red") != pcInfo.dimensions.end() &&
                    std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), "Green") != pcInfo.dimensions.end() &&
                    std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), "Blue") != pcInfo.dimensions.end();
        LOGD << "Has colors: " << (hasColors ? "true" : "false");

        toCloud = std::make_unique<CoordsTransformer>(3857, pcInfo.wktProjection);
        toMercator = std::make_unique<CoordsTransformer>(pcInfo.wktProjection, 3857);

        if (hasColors && !utils::isNetworkPath(inputPath))
        {
            try
            {
                nodeReader = std::make_unique<CopcNodeReader>(inputPath);
                if (!nodeReader->hasColor())
                    nodeReader.reset();
            }
            catch (const std::exception &e)
            {
                LOGW << "Reading " << inputPath << " with PDAL: " << e.what();
            }
        }

#ifdef _WIN32
        const fs::path caBundlePath = io::getDataPath("curl-ca-bundle.crt");
        if (!caBundlePath.empty())
        {
            LOGD << "ARBITRER CA Bundle: " << caBundlePath.string();
            std::stringstream ss;
            ss << "ARBITER_CA_INFO=" << caBundlePath.string();
            if (_putenv(ss.str().c_str()) != 0)
            {
                LOGD << "Cannot set ARBITER_CA_INFO";
            }
        }
#endif
    }

    PointCloudTiler::~PointCloudTiler() = default;

    int PointCloudTiler::resolutionOffset()
    {
        static const int offset = []() {
            const char *value = std::getenv("DDB_PCTILE_RESOLUTION_OFFSET");
            if (value == nullptr || *value == '\0')
                return 2;
            try
            {
                return std::stoi(value);
            }
            catch (const std::exception &)
            {
                LOGW << "Ignoring invalid DDB_PCTILE_RESOLUTION_OFFSET=" << value;
                return 2;
            }
        }();
        return offset;
    }

    std::string PointCloudTiler::tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer, int *outBufferSize)
    {
        const int xyzY = tms ? tmsToXYZ(ty, tz) : ty;

        BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
        if (!tMinMax.contains(tx, xyzY))
            throw GDALException(std::string("Out of bounds [(") +
                                std::to_string(tMinMax.min.x) + "; " +
                                std::to_string(tMinMax.min.y) + ") - (" +
                                std::to_string(tMinMax.max.x) + "; " +
                                std::to_string(tMinMax.max.y) + ")]");

        // Get bounds of tile (3857), convert to the cloud CRS
        const auto tileBounds = mercator.tileBounds(tx, xyzY, tz);
        auto bounds = tileBounds;

        // Expand by a few meters, so that we have sufficient
        // overlap with other tiles
        const double tileResolution = mercator.resolution(tz);
        const int boundsBufSize = tileResolution * std::ceil(static_cast<float>(tileSize) / 40.0); // resolution (m / px) * border (px) --> buffer (m)
        bounds.min.x -= boundsBufSize;
        bounds.max.x += boundsBufSize;
        bounds.min.y -= boundsBufSize;
        bounds.max.y += boundsBufSize;

        toCloud->transform(&bounds.min.x, &bounds.min.y);
        toCloud->transform(&bounds.max.x, &bounds.max.y);

        // readers.copc parses its bounds from this text: the node reader takes the same
        // (rounded) values, so both read the same points
        const auto asText = [](double v) {
            std::ostringstream os;
            os << std::setprecision(14) << v;
            return std::stod(os.str());
        };
        const double minX = asText(bounds.min.x), maxX = asText(bounds.max.x);
        const double minY = asText(bounds.min.y), maxY = asText(bounds.max.y);
        std::stringstream ss;
        ss << std::setprecision(14) << "([" << minX << "," << maxX << "]," << "[" << minY << "," << maxY << "])";
        LOGD << "COPC bounds: " << ss.str();

        const double copcResolution = mercator.resolution(tz - resolutionOffset());
        LOGD << "COPC resolution: " << copcResolution;

        // Discs of pointRadius px; the z-buffer is padded by the radius so that points
        // just outside the tile still draw their part of a disc inside it
        const int pointRadius = 2;
        const double pointRadiusMeters = pointRadius * tileResolution;
        const int paddedTileSize = tileSize + pointRadius * 2;

        PixelMapping mapping;
        mapping.originX = tileBounds.min.x;
        mapping.shiftX = -pointRadiusMeters;
        mapping.scaleX = paddedTileSize / (tileBounds.max.x - tileBounds.min.x + pointRadiusMeters * 2.0);
        mapping.originY = tileBounds.min.y;
        mapping.shiftY = pointRadiusMeters;
        mapping.scaleY = paddedTileSize / (tileBounds.max.y - tileBounds.min.y + pointRadiusMeters * 2.0);
        mapping.width = mapping.height = paddedTileSize;

        std::optional<PointRasterizer> rasterizer;
        const auto draw = [&](PointBatch &points) {
            toMercator->transform(points.size(), points.x.data(), points.y.data());
            rasterizer->add(points);
        };

        bool drawn = false;
        if (nodeReader)
        {
            try
            {
                rasterizer.emplace(tileSize, mapping, pointRadius, pointRadius);
                nodeReader->read(minX, minY, maxX, maxY, copcResolution, draw);
                drawn = true;
            }
            catch (const std::exception &e)
            {
                LOGW << "COPC node read failed for " << inputPath << ", reading with PDAL: " << e.what();
            }
        }
        if (!drawn)
        {
            rasterizer.emplace(tileSize, mapping, pointRadius, pointRadius);
            readWithPdal(ss.str(), copcResolution, draw);
        }

        LOGD << "Drew " << rasterizer->drawn() << " points";

        return writeTile(rasterizer->toImage(), format, tz, tx, ty, outBuffer, outBufferSize);
    }

    void PointCloudTiler::readWithPdal(const std::string &bounds, double resolution,
                                       const std::function<void(PointBatch &)> &sink)
    {
        pdal::Options copcOpts = copcReaderOptions(inputPath);
        copcOpts.add("bounds", bounds);
        copcOpts.add("resolution", resolution);

        pdal::CopcReader copcReader;
        copcReader.setOptions(copcOpts);
        pdal::Stage *main = &copcReader;

        std::unique_ptr<pdal::ColorinterpFilter> colorFilter;
        if (!hasColors)
        {
            try
            {
                colorFilter = std::make_unique<pdal::ColorinterpFilter>();

                // Add ramp filter
                LOGD << "Adding ramp filter (" << pcInfo.bounds[2] << ", " << pcInfo.bounds[5] << ")";

                pdal::Options cfOpts;
                cfOpts.add("ramp", "pestel_shades");
                cfOpts.add("minimum", pcInfo.bounds[2]);
                cfOpts.add("maximum", pcInfo.bounds[5]);
                colorFilter->setOptions(cfOpts);
                colorFilter->setInput(copcReader);
                main = colorFilter.get();
            }
            catch (const pdal::pdal_error &e)
            {
                LOGD << "ColorinterpFilter failed: " << e.what() << ". Proceeding without color filter.";
                colorFilter.reset();
                main = &copcReader;
            }
        }

        try
        {
            forEachPointBatch(*main, sink);
        }
        catch (const pdal::pdal_error &e)
        {
            throw PDALException(e.what());
        }
    }

} // namespace ddb
