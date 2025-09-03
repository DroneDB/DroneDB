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

namespace ddb
{

    EptTiler::EptTiler(const std::string &inputPath, const std::string &outputFolder,
                       int tileSize, bool tms)
        : Tiler(inputPath, outputFolder, tileSize, tms),
          wSize(tileSize * tileSize)
    {

        // Open EPT
        int span;
        if (!getEptInfo(inputPath, eptInfo, 3857, &span))
        {
            throw InvalidArgsException("Cannot get EPT info for " + inputPath);
        }

        if (eptInfo.wktProjection.empty())
        {
            throw InvalidArgsException("EPT file has no WKT SRS: " + inputPath);
        }

        oMinX = eptInfo.polyBounds.getPoint(0).y;
        oMaxX = eptInfo.polyBounds.getPoint(2).y;
        oMaxY = eptInfo.polyBounds.getPoint(2).x;
        oMinY = eptInfo.polyBounds.getPoint(0).x;

        LOGD << "Bounds (output SRS): (" << oMinX << "; " << oMinY << ") - ("
             << oMaxX << "; " << oMaxY << ")";

        // Max/min zoom level
        tMinZ = mercator.zoomForLength(std::min(oMaxX - oMinX, oMaxY - oMinY));
        tMaxZ = tMinZ + static_cast<int>(std::round(std::log(static_cast<double>(span) / 4.0) / std::log(2)));

        LOGD << "MinZ: " << tMinZ;
        LOGD << "MaxZ: " << tMaxZ;

        hasColors = std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Red") != eptInfo.dimensions.end() &&
                    std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Green") != eptInfo.dimensions.end() &&
                    std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Blue") != eptInfo.dimensions.end();
        LOGD << "Has colors: " << (hasColors ? "true" : "false");

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

    EptTiler::~EptTiler()
    {
    }

    std::string EptTiler::tile(int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize)
    {
        std::string tilePath = getTilePath(tz, tx, ty, true);

        if (tms)
        {
            ty = tmsToXYZ(ty, tz);
            LOGD << "TY: " << ty;
        }

        BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
        if (!tMinMax.contains(tx, ty))
            throw GDALException(std::string("Out of bounds [(") +
                                std::to_string(tMinMax.min.x) + "; " +
                                std::to_string(tMinMax.min.y) + ") - (" +
                                std::to_string(tMinMax.max.x) + "; " +
                                std::to_string(tMinMax.max.y) + ")]");

        // Get bounds of tile (3857), convert to EPT CRS
        auto tileBounds = mercator.tileBounds(tx, ty, tz);
        auto bounds = tileBounds;

        // Expand by a few meters, so that we have sufficient
        // overlap with other tiles
        double tileResolution = mercator.resolution(tz);
        const int boundsBufSize = tileResolution * std::ceil(static_cast<float>(tileSize) / 40.0); // resolution (m / px) * border (px) --> buffer (m)
        bounds.min.x -= boundsBufSize;
        bounds.max.x += boundsBufSize;
        bounds.min.y -= boundsBufSize;
        bounds.max.y += boundsBufSize;

        CoordsTransformer ct(3857, eptInfo.wktProjection);
        ct.transform(&bounds.min.x, &bounds.min.y);
        ct.transform(&bounds.max.x, &bounds.max.y);

        pdal::Options eptOpts;
        fs::path path(inputPath);
        eptOpts.add("filename", (!utils::isNetworkPath(inputPath) && path.is_relative()) ? ("." / path).string() : inputPath);

        std::stringstream ss;
        ss << std::setprecision(14) << "([" << bounds.min.x << "," << bounds.min.y << "], " << "[" << bounds.max.x << "," << bounds.max.y << "])";
        eptOpts.add("bounds", ss.str());
        LOGD << "EPT bounds: " << ss.str();

        double eptResolution = mercator.resolution(tz - 2);
        eptOpts.add("resolution", eptResolution);
        LOGD << "EPT resolution: " << eptResolution;

        std::unique_ptr<pdal::EptReader> eptReader = std::make_unique<pdal::EptReader>();
        pdal::Stage *main = eptReader.get();
        eptReader->setOptions(eptOpts);
        LOGD << "Options set";

        std::unique_ptr<pdal::ColorinterpFilter> colorFilter;
        if (!hasColors)
        {
            colorFilter.reset(new pdal::ColorinterpFilter());

            // Add ramp filter
            LOGD << "Adding ramp filter (" << eptInfo.bounds[2] << ", " << eptInfo.bounds[5] << ")";

            pdal::Options cfOpts;
            cfOpts.add("ramp", "pestel_shades");
            cfOpts.add("minimum", eptInfo.bounds[2]);
            cfOpts.add("maximum", eptInfo.bounds[5]);
            colorFilter->setOptions(cfOpts);
            colorFilter->setInput(*eptReader);
            main = colorFilter.get();
        }

        pdal::PointTable table;
        main->prepare(table);
        pdal::PointViewSet point_view_set;

        LOGD << "PointTable prepared";

        try
        {
            point_view_set = main->execute(table);
        }
        catch (const pdal::pdal_error &e)
        {
            throw PDALException(e.what());
        }

        pdal::PointViewPtr point_view = *point_view_set.begin();
        pdal::Dimension::IdList dims = point_view->dims();

        const int nBands = 3;
        const int bufSize = GDALGetDataTypeSizeBytes(GDT_Byte) * wSize;

        const int pointRadius = 2;
        const double pointRadiusMeters = pointRadius * tileResolution;
        const int paddedTileSize = tileSize + pointRadius * 2;
        const int paddedWSize = paddedTileSize * paddedTileSize;
        const int paddedBufSize = GDALGetDataTypeSizeBytes(GDT_Byte) * paddedWSize;

        std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufSize * nBands]);
        std::unique_ptr<uint8_t[]> alphaBuffer(new uint8_t[bufSize]);
        std::unique_ptr<float[]> zBuffer(new float[paddedBufSize]);

        memset(buffer.get(), 0, bufSize * nBands);
        memset(alphaBuffer.get(), 0, bufSize);

        for (int i = 0; i < paddedWSize; i++)
        {
            zBuffer.get()[i] = -99999.0;
        }

        LOGD << "Fetched " << point_view->size() << " points";

        const double tileScaleW = tileSize / (tileBounds.max.x - tileBounds.min.x);
        const double tileScaleH = tileSize / (tileBounds.max.y - tileBounds.min.y);
        const double paddedTileScaleW = paddedTileSize / (tileBounds.max.x - tileBounds.min.x + pointRadiusMeters * 2.0);
        const double paddedTileScaleH = paddedTileSize / (tileBounds.max.y - tileBounds.min.y + pointRadiusMeters * 2.0);

        CoordsTransformer ict(eptInfo.wktProjection, 3857);

        bool normalize = false;
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx)
        {
            auto p = point_view->point(idx);
            uint16_t red = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Red);
            uint16_t green = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Green);
            uint16_t blue = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Blue);

            if (red > 255 || green > 255 || blue > 255)
            {
                normalize = true;
                break;
            }
        }

        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx)
        {
            auto p = point_view->point(idx);
            double x = p.getFieldAs<double>(pdal::Dimension::Id::X);
            double y = p.getFieldAs<double>(pdal::Dimension::Id::Y);
            double z = p.getFieldAs<double>(pdal::Dimension::Id::Z);

            ict.transform(&x, &y);

            // Map projected coordinates to local PNG coordinates
            int px = std::round((x - tileBounds.min.x - pointRadiusMeters) * paddedTileScaleW);
            int py = paddedTileSize - 1 - std::round((y - tileBounds.min.y + pointRadiusMeters) * paddedTileScaleH);

            if (px >= 0 && px < paddedTileSize && py >= 0 && py < paddedTileSize)
            {
                // Within bounds
                PointColor color;
                uint16_t red = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Red);
                uint16_t green = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Green);
                uint16_t blue = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Blue);

                if (normalize)
                {
                    color.r = red >> 8;
                    color.g = green >> 8;
                    color.b = blue >> 8;
                }
                else
                {
                    color.r = static_cast<uint8_t>(red);
                    color.g = static_cast<uint8_t>(green);
                    color.b = static_cast<uint8_t>(blue);
                }

                int off_px = px - pointRadius;
                int off_py = py - pointRadius;

                if (zBuffer.get()[py * paddedTileSize + px] < z)
                {
                    zBuffer.get()[py * paddedTileSize + px] = z;
                    drawCircle(buffer.get(), alphaBuffer.get(), off_px, off_py,
                               pointRadius, color.r, color.g, color.b, tileSize, wSize);
                }
            }
        }

        GDALDriverH memDrv = GDALGetDriverByName("MEM");
        if (memDrv == nullptr)
            throw GDALException("Cannot create MEM driver");
        GDALDriverH pngDrv = GDALGetDriverByName("PNG");
        if (pngDrv == nullptr)
            throw GDALException("Cannot create PNG driver");

        // Need to create in-memory dataset
        // (PNG driver does not have Create() method)
        const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, nBands + 1,
                                               GDT_Byte, nullptr);
        if (dsTile == nullptr)
            throw GDALException("Cannot create dsTile");

        if (GDALDatasetRasterIO(dsTile, GF_Write, 0, 0,
                                tileSize, tileSize,
                                buffer.get(), tileSize, tileSize,
                                GDT_Byte, nBands, nullptr, 0, 0,
                                0) != CE_None)
        {
            throw GDALException("Cannot write tile data");
        }

        const GDALRasterBandH tileAlphaBand =
            GDALGetRasterBand(dsTile, nBands + 1);
        GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

        if (GDALRasterIO(tileAlphaBand, GF_Write, 0, 0, tileSize, tileSize,
                         alphaBuffer.get(), tileSize, tileSize,
                         GDT_Byte, 0, 0) != CE_None)
        {
            throw GDALException("Cannot write tile alpha data");
        }

        const GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                                  nullptr, nullptr, nullptr);
        if (outDs == nullptr)
            throw GDALException("Cannot create output dataset " + tilePath);

        GDALFlushCache(outDs);
        GDALClose(outDs);
        GDALClose(dsTile);

        if (outBuffer != nullptr)
        {
            vsi_l_offset bufSize;
            *outBuffer = VSIGetMemFileBuffer(tilePath.c_str(), &bufSize, TRUE);
            if (bufSize > std::numeric_limits<int>::max())
                throw GDALException("Exceeded max buf size");
            *outBufferSize = bufSize;
            return "";
        }
        else
        {
            return tilePath;
        }
    }

    void drawCircle(uint8_t *buffer, uint8_t *alpha, int px, int py, int radius,
                    uint8_t r, uint8_t g, uint8_t b, int tileSize, int wSize)
    {
        const int r2 = radius * radius;
        const int area = r2 << 2;
        const int rr = radius << 1;

        for (int i = 0; i < area; i++)
        {
            const int tx = (i % rr) - radius;
            const int ty = (i / rr) - radius;
            if (tx * tx + ty * ty <= r2)
            {
                const int dx = px + tx;
                const int dy = py + ty;
                if (dx >= 0 && dx < tileSize && dy >= 0 && dy < tileSize)
                {
                    buffer[dy * tileSize + dx + wSize * 0] = r;
                    buffer[dy * tileSize + dx + wSize * 1] = g;
                    buffer[dy * tileSize + dx + wSize * 2] = b;
                    alpha[dy * tileSize + dx] = 255;
                }
            }
        }
    }

} // namespace ddb
