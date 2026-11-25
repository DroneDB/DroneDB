/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The tile of a photo, drawn by the raster helper from the photo itself for each request (raster_ops.h)
#include "raster_ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "cancellation.h"
#include "embedded_preview.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "image_translation.h"
#include "mio.h"
#include "photo_tile.h"
#include "raster_stats.h"
#include "tiler.h"

namespace ddb::raster
{
    namespace
    {
        // The level of the photo a tile is drawn from, opened with the open policy
        struct PhotoLevel
        {
            GDALDatasetUniquePtr dataset;
            std::string path;
        };

        // How many times the pixels a tile needs a JPEG preview must hold to be drawn from. A camera makes
        // its preview sharper than an average of the photo (the 960 x 640 preview of a DJI photo has half as
        // much contrast again in its fine detail): a tile drawn from a preview reduced by 1.2 differed from
        // the tile of the COG of the photo by 18 to 22 levels on average, by 7 to 11 reduced by 2, by 5 to 7
        // reduced by 4, as much as a tile drawn from the photo itself (5 to 7).
        constexpr int kPreviewOversampling = 4;

        // The smallest level of the photo that holds `longSide` pixels on its long side: a JPEG preview of
        // the same framing (the 960 x 640 copy of DJI photos) when one is kPreviewOversampling times that
        // large, since the entropy decoding of the whole picture is most of the cost of a tile; else the
        // photo, with its own overviews (the DCT scales of a JPEG, overviewFor)
        PhotoLevel openPhotoLevel(const fs::path &photo, int longSide)
        {
            if (io::Path(photo).checkExtension({"jpg", "jpeg"}))
            {
                const JpegLayout layout = readJpegLayout(photo);
                if (const auto preview = pickJpegPreview(layout, longSide * kPreviewOversampling))
                {
                    std::string path = jpegPreviewGdalPath(photo, *preview);
                    if (auto dataset = openUserRaster(path))
                        return {std::move(dataset), std::move(path)};
                }
            }
            std::string path = io::toUtf8(photo);
            auto dataset = openUserRaster(path);
            if (!dataset)
                throw RasterOpException(ErrorCode::Unreadable, "Cannot open " + path + " for reading");
            return {std::move(dataset), std::move(path)};
        }

        // A virtual copy of `level` placed on the ground by the corners of the footprint as ground control
        // points (photoGcps: the order of the COG builds of a photo), scaled to the size of the level
        GDALDatasetUniquePtr placedOnTheGround(const PhotoLevel &level, const PhotoGeoreference &photo)
        {
            GDALDatasetH source = datasetHandle(level.dataset);
            auto vrt = adoptDataset(
                GDALCreateCopy(GDALGetDriverByName("VRT"), "", source, FALSE, nullptr, nullptr, nullptr));
            if (!vrt)
                throw GDALException("Cannot read " + level.path + ": " + CPLGetLastErrorMsg());

            const double sx = static_cast<double>(GDALGetRasterXSize(source)) / photo.width;
            const double sy = static_cast<double>(GDALGetRasterYSize(source)) / photo.height;
            const json corners = photoGcps(photo, 1.0);
            std::vector<GDAL_GCP> gcps(corners.size());
            GDALInitGCPs(static_cast<int>(gcps.size()), gcps.data());
            for (size_t i = 0; i < gcps.size(); i++)
            {
                gcps[i].dfGCPPixel = corners[i][0].get<double>() * sx;
                gcps[i].dfGCPLine = corners[i][1].get<double>() * sy;
                gcps[i].dfGCPX = corners[i][2].get<double>();
                gcps[i].dfGCPY = corners[i][3].get<double>();
            }
            const SrsPtr wgs84 = srsFromEpsg(4326);
            const CPLErr set = GDALSetGCPs2(datasetHandle(vrt), static_cast<int>(gcps.size()), gcps.data(), wgs84.get());
            GDALDeinitGCPs(static_cast<int>(gcps.size()), gcps.data());
            if (set != CE_None)
                throw GDALException("Cannot place " + level.path + " on the ground: " + CPLGetLastErrorMsg());
            return vrt;
        }

        // The overview of `placed` the tile is warped from, as gdalwarp -ovr takes it: the smallest with the
        // framing of the photo that holds `longSide` pixels on its long side, "NONE" (the full resolution)
        // when none does. GDAL lists the EXIF thumbnail of a JPEG among its overviews, and its framing is
        // not always the photo's (the 160 x 120 thumbnail of a 16:9 DJI photo has black bands): gdalwarp's
        // own choice took it for the tiles of the zoom levels where the photo is that small.
        std::string overviewFor(GDALDatasetH placed, int longSide)
        {
            GDALRasterBandH band = GDALGetRasterBand(placed, 1);
            const double aspect =
                static_cast<double>(GDALGetRasterXSize(placed)) / static_cast<double>(GDALGetRasterYSize(placed));
            int chosen = -1, chosenSide = 0;
            for (int i = 0; i < GDALGetOverviewCount(band); i++)
            {
                GDALRasterBandH overview = GDALGetOverview(band, i);
                const int width = GDALGetRasterBandXSize(overview), height = GDALGetRasterBandYSize(overview);
                const int side = std::max(width, height);
                // The proportions of the photo, within the rounding of an overview of a few pixels (1 %)
                const bool sameFraming =
                    height > 0 && std::abs(static_cast<double>(width) / height - aspect) <= 0.01 * aspect;
                if (sameFraming && side >= longSide && (chosen < 0 || side < chosenSide))
                {
                    chosen = i;
                    chosenSide = side;
                }
            }
            return chosen < 0 ? std::string("NONE") : std::to_string(chosen);
        }

        // The tile warped from the placed photo: its bands and an alpha band last, transparent outside the
        // footprint. The homography of the four corners, as the COG builds of a photo use; bilinear (the
        // nearest pixel for a palette) from the overview with `longSide` pixels (overviewFor).
        GDALDatasetUniquePtr warpTile(GDALDatasetH placed, const BoundingBox<Projected2D> &box, int size,
                                      int longSide, bool palette, const std::string &label)
        {
            const std::vector<std::string> args = {
                "-of", "MEM", "-t_srs", "EPSG:3857",
                "-te", utilityNumber(box.min.x), utilityNumber(box.min.y),
                utilityNumber(box.max.x), utilityNumber(box.max.y),
                "-ts", std::to_string(size), std::to_string(size),
                "-r", palette ? "near" : "bilinear",
                "-dstalpha",
                "-ovr", overviewFor(placed, longSide),
                "-to", "METHOD=GCP_HOMOGRAPHY"};
            CPLStringList argv = cslFromStrings(args);
            const GdalOptionsPtr<GDALWarpAppOptions> options(GDALWarpAppOptionsNew(argv.List(), nullptr));
            if (!options)
                throw GDALException("Cannot set up the warp of " + label);
            cancel::GdalProgress progress;
            progress.attach(options.get());
            GDALDatasetH sources[] = {placed};
            auto warped = adoptDataset(GDALWarp("", nullptr, 1, sources, options.get(), nullptr));
            if (!warped)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot warp " + label + " to a tile: " + CPLGetLastErrorMsg());
            }
            return warped;
        }
    } // namespace

    RasterImage renderPhotoTile(const fs::path &path, const PhotoTileParams &params)
    {
        const int size = params.tileSize;
        const BoundingBox<Projected2D> box = photoTileBounds(params);
        const double tileResolution = GlobalMercator(size).resolution(params.z);

        // A tile that holds nothing of the photo is blank: the photo is not opened (a pixel of margin)
        if (!footprintMeetsBox(params, box, tileResolution))
            return RasterImage(size, size, 3, true);

        // The pixels of the photo on the long side of the tile's zoom
        const double longSide = static_cast<double>(std::max(params.width, params.height));
        const int needed = static_cast<int>(
            std::clamp(std::ceil(longSide * photoResolution(params) / tileResolution), 1.0, longSide));
        const PhotoLevel level = openPhotoLevel(path, needed);
        GDALDatasetH source = datasetHandle(level.dataset);
        requireDecodableLevels(source, level.path);

        GDALRasterBandH first = GDALGetRasterBand(source, 1);
        if (first == nullptr)
            throw RasterOpException(ErrorCode::Unreadable, level.path + " has no band");
        const bool palette = GDALGetRasterColorTable(first) != nullptr;
        const bool isByte = GDALGetRasterDataType(first) == GDT_Byte;

        const auto placed = placedOnTheGround(level, params);
        const auto warped = warpTile(datasetHandle(placed), box, size, needed, palette, level.path);
        GDALDatasetH tile = datasetHandle(warped);

        // Grey is replicated to RGB, else the first three bands; the alpha band of the warp is the last.
        // Data that is not 8-bit is stretched over the minimum to the maximum of each band of the whole
        // photo, as its thumbnail: every tile of the photo gets the same stretch.
        const int alphaBand = GDALGetRasterCount(tile);
        const std::vector<int> bands = alphaBand - 1 <= 2 ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
        std::vector<imaging::ScaleRange> scales;
        if (!palette && !isByte)
            for (const auto &s : RasterStatsProvider::instance().getStretchStatistics(level.path, source, bands))
                scales.push_back({s.min, s.max});

        const imaging::AlphaSource alpha{alphaBand, false};
        RasterImage image = imaging::translateToImage(tile, {size, size, bands, palette, scales, level.path, alpha});
        if (palette)
            imaging::readPaletteAlpha(tile, alpha, image);
        return image;
    }

    void photoTile(const fs::path &path, const fs::path &outFile, const PhotoTileParams &params)
    {
        EncodeOptions options;
        options.format = params.format;
        TileEncoder::write(renderPhotoTile(path, params), options, io::toUtf8(outFile));
    }

} // namespace ddb::raster
