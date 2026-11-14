/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "raster_tiler.h"

#include <cstdint>

#include "exceptions.h"
#include "json.h"
#include "raster_artefacts.h"
#include "thumbs.h"
#include "tilers_call.h"

namespace ddb
{
    namespace
    {
        const char *optional(const std::string &s) { return s.empty() ? nullptr : s.c_str(); }

        // Whether tile (tx, ty) exists at zoom tz (ddb-tilers refuses the others; the tilers of
        // libddb answer them as tiles out of the raster)
        bool inWorld(int tz, int tx, int ty)
        {
            if (tz < 0 || tz > 30 || tx < 0 || ty < 0)
                return false;
            const int64_t n = int64_t{1} << tz;
            return tx < n && ty < n;
        }

        bool hasAnyParameter(const ThumbVisParams &v)
        {
            return !v.preset.empty() || !v.bands.empty() || !v.formula.empty() || !v.bandFilter.empty() ||
                   !v.colormap.empty() || !v.rescale.empty() || v.nodata.has_value();
        }
    } // namespace

    RasterTiler::RasterTiler(const std::string &cogPath, const std::string &tilesFolder, int size, bool tmsRows)
        : Tiler(cogPath, tilesFolder, size, tmsRows)
    {
        raster::requireArtefact(io::fromUtf8(cogPath));
        char *raw = nullptr;
        tilers::check(ddbtTileInfo(cogPath.c_str(), size, &raw));
        const tilers::String text(raw);
        const json info = json::parse(text.get());
        oMinX = info.at("minX").get<double>();
        oMinY = info.at("minY").get<double>();
        oMaxX = info.at("maxX").get<double>();
        oMaxY = info.at("maxY").get<double>();
        tMinZ = info.at("minZoom").get<int>();
        tMaxZ = info.at("maxZoom").get<int>();
        nBands = 0; // not used: ddb-tilers chooses the bands of every tile
    }

    std::string RasterTiler::tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer, int *outBufferSize)
    {
        // Out of the world is out of the raster: the transparent tile
        if (!inWorld(tz, tx, ty))
            return writeTile(RasterImage(tileSize, tileSize, 3, true), format, tz, tx, ty, outBuffer, outBufferSize);

        uint8_t *raw = nullptr;
        int size = 0;
        tilers::check(ddbtMemoryTileFmt(inputPath.c_str(), tz, tx, ty, &raw, &size, tileSize, tms, false, nullptr,
                                        imageFormatExtension(format)));
        const tilers::Bytes bytes(raw);
        return writeEncodedTile(bytes.get(), static_cast<size_t>(size), format, tz, tx, ty, outBuffer, outBufferSize);
    }

    std::string RasterTiler::tile(int tz, int tx, int ty, const ThumbVisParams &visParams, ImageFormat format,
                                  uint8_t **outBuffer, int *outBufferSize)
    {
        // A tile out of the world, as one out of the raster: refused with parameters, else transparent
        if (!inWorld(tz, tx, ty))
        {
            if (hasAnyParameter(visParams))
                throw GDALException("Out of bounds");
            return tile(tz, tx, ty, format, outBuffer, outBufferSize);
        }

        // The rescale of visParams is already in the unit of its formula, so ddb-tilers converts nothing
        ddbtRasterOptions options{};
        options.structSize = sizeof(options);
        if (visParams.nodata)
        {
            options.flags |= DDBT_RASTER_OPT_NODATA;
            options.nodata = *visParams.nodata;
        }
        uint8_t *raw = nullptr;
        int size = 0;
        tilers::check(ddbtMemoryTileEx3(inputPath.c_str(), tz, tx, ty, tileSize, tms, false, nullptr,
                                        optional(visParams.preset), optional(visParams.bands),
                                        optional(visParams.formula), optional(visParams.bandFilter),
                                        optional(visParams.colormap), optional(visParams.rescale), &options,
                                        imageFormatExtension(format), &raw, &size));
        const tilers::Bytes bytes(raw);
        return writeEncodedTile(bytes.get(), static_cast<size_t>(size), format, tz, tx, ty, outBuffer, outBufferSize);
    }
} // namespace ddb
