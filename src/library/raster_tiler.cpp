/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "raster_tiler.h"

#include <array>

#include "exceptions.h"
#include "raster_artefacts.h"
#include "thumbs.h"
#include "tilers_call.h"
#include "tilers_raster.h"

namespace ddb
{
    namespace
    {
        const char *optional(const std::string &s) { return s.empty() ? nullptr : s.c_str(); }

        bool hasAnyParameter(const ThumbVisParams &v)
        {
            return !v.preset.empty() || !v.bands.empty() || !v.formula.empty() || !v.bandFilter.empty() ||
                   !v.colormap.empty() || !v.rescale.empty() || v.nodata.has_value();
        }

        // cogPath, once it is known to be an artefact (before ddb-tilers opens it)
        const std::string &artefact(const std::string &cogPath)
        {
            raster::requireArtefact(io::fromUtf8(cogPath));
            return cogPath;
        }
    } // namespace

    RasterTiler::RasterTiler(const std::string &cogPath, const std::string &tilesFolder, int size, bool tmsRows)
        : TilersTiler(artefact(cogPath), tilesFolder, size, tmsRows)
    {
    }

    RasterTiler::~RasterTiler()
    {
        // ddb-tilers keeps the files it tiles open between tiles: the COG of a request folder is
        // closed before the folder goes (Windows removes no open file); the other files open again
        // on their next tile
        if (request_)
            ddbtReleaseSources();
    }

    void RasterTiler::keep(std::unique_ptr<raster::RequestFolder> request)
    {
        request_ = std::move(request);
    }

    std::optional<BoundingBox<Projected2D>> RasterTiler::contentBounds() const
    {
        std::call_once(contentOnce, [this] {
            const auto raster = tilers::Raster::open(io::fromUtf8(inputPath));
            if (!raster->geoTransform())
            {
                content = TilersTiler::contentBounds();
                return;
            }
            const std::optional<BoundingBox<Projected2Di>> window = raster->opaqueWindow();
            if (!window)
                return;
            // An artefact is in EPSG:3857, north up: its pixel (x, y) starts at gt[0] + x * gt[1], gt[3] + y * gt[5]
            const std::array<double, 6> &gt = *raster->geoTransform();
            content = BoundingBox<Projected2D>(
                Projected2D(gt[0] + window->min.x * gt[1], gt[3] + (window->max.y + 1) * gt[5]),
                Projected2D(gt[0] + (window->max.x + 1) * gt[1], gt[3] + window->min.y * gt[5]));
        });
        return content;
    }

    std::string RasterTiler::tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer, int *outBufferSize)
    {
        // Out of the world is out of the raster: the transparent tile (ddb-tilers refuses such a tile)
        if (!tileInWorld(tz, tx, ty))
            return writeTile(RasterImage(tileSize, tileSize, 3, true), format, tz, tx, ty, outBuffer, outBufferSize);
        return TilersTiler::tile(tz, tx, ty, format, outBuffer, outBufferSize);
    }

    std::string RasterTiler::tile(int tz, int tx, int ty, const ThumbVisParams &visParams, ImageFormat format,
                                  uint8_t **outBuffer, int *outBufferSize)
    {
        // A tile out of the world, as one out of the raster: refused with parameters, else transparent
        if (!tileInWorld(tz, tx, ty))
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
