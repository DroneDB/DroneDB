/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The thumbnail operations of the raster helper: the GDAL part of the thumbnail of a photo or a
// raster (raster_ops.h), run in the helper process, which writes the file.
#include "raster_ops.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

#include "cancellation.h"
#include "embedded_preview.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "image_translation.h"
#include "logger.h"
#include "mio.h"
#include "raster_stats.h"
#include "raster_utils.h"
#include "thumbs.h"
#include "thermal.h"
#include "thumb_internal.h"
#include "vis_render.h"

#include <gdal_vrt.h>
#include <vrtdataset.h>

namespace ddb {

namespace {

using namespace imaging;

// Opens the file of the request: always a local file, the host downloads a URL before it asks
GDALDatasetUniquePtr openThumbSource(const fs::path& imagePath, std::string& resolvedPath) {
    resolvedPath = io::toUtf8(imagePath);
    auto src = openUserRaster(resolvedPath);
    if (!src)
        throw raster::RasterOpException(raster::ErrorCode::Unreadable, "Cannot open " + resolvedPath + " for reading");
    return src;
}

// Opens the source of an image thumbnail. A local JPEG that carries a preview of the same
// framing at least thumbSize pixels long (MPF, e.g. the 960x720 copy in DJI photos) is
// decoded from the preview: the entropy decoding of the full image is most of the cost of
// a thumbnail, whatever the DCT scale. frameWidth/frameHeight receive the size of the image
// the thumbnail represents (the main image), so both sources give the same output size.
GDALDatasetUniquePtr openImageThumbSource(const fs::path& imagePath, int thumbSize, std::string& openPath,
                                          int& frameWidth, int& frameHeight) {
    frameWidth = frameHeight = 0;
    if (io::Path(imagePath).checkExtension({"jpg", "jpeg"})) {
        const JpegLayout layout = readJpegLayout(imagePath);
        if (const auto preview = pickJpegPreview(layout, thumbSize)) {
            openPath = jpegPreviewGdalPath(imagePath, *preview);
            if (auto previewDs = openUserRaster(openPath)) {
                frameWidth = layout.width;
                frameHeight = layout.height;
                LOGD << "Thumbnail of " << io::toUtf8(imagePath) << " from its " << preview->width << "x"
                     << preview->height << " preview";
                return previewDs;
            }
        }
    }

    auto src = openThumbSource(imagePath, openPath);
    frameWidth = src->GetRasterXSize();
    frameHeight = src->GetRasterYSize();
    return src;
}

// The bands of a complex raster as their tiles and statistics show them: the magnitude
// sqrt(re^2 + im^2) of each sample, Float64 bands of a VRT that reads `src` (which the caller keeps
// open), with its NoData value and its mask; nullptr when the raster is not complex. A smaller
// image takes the magnitude of the nearest sample, as the overviews the tiles read do: an average of
// complex samples cancels their phases (and the VRT resamples its sources before the magnitude)
GDALDatasetUniquePtr complexMagnitudesOf(GDALDatasetH src) {
    const int bandCount = GDALGetRasterCount(src);
    if (bandCount <= 0 || !GDALDataTypeIsComplex(GDALGetRasterDataType(GDALGetRasterBand(src, 1))))
        return nullptr;
    const int w = GDALGetRasterXSize(src), h = GDALGetRasterYSize(src);
    GDALDatasetUniquePtr vrt(GDALDataset::FromHandle(VRTCreate(w, h)));
    GDALDatasetH hVrt = datasetHandle(vrt);
    const char* const derived[] = {"subClass=VRTDerivedRasterBand", "PixelFunctionType=mod", nullptr};
    for (int b = 1; b <= bandCount; ++b) {
        GDALRasterBandH from = GDALGetRasterBand(src, b);
        if (GDALAddBand(hVrt, GDT_Float64, const_cast<char**>(derived)) != CE_None)
            throw GDALException("Cannot read the magnitudes of the complex band " + std::to_string(b));
        GDALRasterBandH to = GDALGetRasterBand(hVrt, b);
        // The C API ignores a resampling other than an average: the source is set to the nearest
        VRTAddSimpleSource(to, from, 0, 0, w, h, 0, 0, w, h, nullptr, VRT_NODATA_UNSET);
        auto* sources = static_cast<VRTSourcedRasterBand*>(GDALRasterBand::FromHandle(to));
        if (auto* simple = dynamic_cast<VRTSimpleSource*>(sources->m_papoSources.back().get()))
            simple->SetResampling("near");
        int hasNodata = FALSE;
        const double nodata = GDALGetRasterNoDataValue(from, &hasNodata);
        if (hasNodata)
            GDALSetRasterNoDataValue(to, std::abs(nodata));
    }
    GDALRasterBandH first = GDALGetRasterBand(src, 1);
    if (GDALGetMaskFlags(first) == GMF_PER_DATASET && GDALCreateDatasetMaskBand(hVrt, GMF_PER_DATASET) == CE_None)
        VRTAddSimpleSource(GDALGetMaskBand(GDALGetRasterBand(hVrt, 1)), GDALGetMaskBand(first), 0, 0, w, h, 0, 0, w,
                           h, nullptr, VRT_NODATA_UNSET);
    return vrt;
}

// Refuses up front a raster whose thumbnail would decode blocks larger than a read may
// (raster::requireDecodableBlocks), before a pixel is read: at the overview GDAL resamples the
// thumbnail from (by average, or by the nearest pixel for a palette, which may take a smaller one).
// A thumbnail that reads a sample of the raster too (the statistics of a stretch, the range of a
// formula or of a colormap) may read it at any level, from an overview smaller than the thumbnail
// to the full resolution, so all of them count. `ds` is the dataset GDAL decodes, not a VRT over it.
void requireThumbnailBlocks(GDALDatasetH ds, int targetWidth, int targetHeight, bool sampled,
                            const std::string& path) {
    if (sampled) {
        raster::requireDecodableLevels(ds, path);
        return;
    }
    GDALRasterBandH first = GDALGetRasterBand(ds, 1);
    if (!first)
        return;
    const int average = raster::overviewReadFor(first, targetWidth, targetHeight, GRIORA_Average);
    const int nearest = raster::overviewReadFor(first, targetWidth, targetHeight, GRIORA_NearestNeighbour);
    raster::requireDecodableBlocks(ds, average, path);
    if (nearest != average)
        raster::requireDecodableBlocks(ds, nearest, path);
}

// The size of a pixel of `ds` on the ground, its height over its width, from the geotransform: 1
// without one (a photo), or with a degenerate one. A raster of 5 x 5000 pixels of 240 x 0.24 m
// covers a square, as its thumbnail must show it
double pixelAspectOf(GDALDatasetH ds) {
    double gt[6];
    if (GDALGetGeoTransform(ds, gt) != CE_None)
        return 1.0;
    const double width = std::hypot(gt[1], gt[4]);
    const double height = std::hypot(gt[2], gt[5]);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        return 1.0;
    return height / width;
}

// Compute thumbnail target dimensions preserving the aspect ratio of an image of
// srcWidth x srcHeight pixels whose pixels are pixelAspect times as high as wide.
// Guaranteed to return values >= 1 pixel.
void computeThumbTargetSize(int srcWidth, int srcHeight, int thumbSize,
                            int& outWidth, int& outHeight, double pixelAspect = 1.0) {
    if (srcWidth <= 0 || srcHeight <= 0 || thumbSize <= 0 || !(pixelAspect > 0))
        throw InvalidArgsException("Invalid thumbnail size parameters");

    const double width = static_cast<double>(srcWidth);
    const double height = static_cast<double>(srcHeight) * pixelAspect;
    const double longest = std::max(width, height);
    const auto side = [&](double length) {
        return static_cast<int>(std::clamp(static_cast<double>(thumbSize) * length / longest, 1.0,
                                           static_cast<double>(thumbSize)));
    };
    outWidth = width >= height ? thumbSize : side(width);
    outHeight = width >= height ? side(height) : thumbSize;
}

// The thumbnail of a photo or a raster: its colours, or the first bands stretched to 0-255 for
// data that is not 8-bit
RasterImage renderPlainThumbnail(const fs::path& imagePath, int thumbSize) {
    std::string openPath;
    int frameWidth = 0, frameHeight = 0;
    const auto src = openImageThumbSource(imagePath, thumbSize, openPath, frameWidth, frameHeight);
    const auto magnitudes = complexMagnitudesOf(datasetHandle(src));
    GDALDatasetH hSrc = datasetHandle(magnitudes ? magnitudes : src);

    const int srcWidth = GDALGetRasterXSize(hSrc);
    const int srcHeight = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    if (srcWidth <= 0 || srcHeight <= 0 || bandCount <= 0 || frameWidth <= 0 || frameHeight <= 0)
        throw GDALException("Invalid source raster dimensions in " + openPath);

    GDALRasterBandH hBand1 = GDALGetRasterBand(hSrc, 1);
    const bool hasPalette = hBand1 && GDALGetRasterColorTable(hBand1) != nullptr;
    const bool isByte = GDALGetRasterDataType(hBand1) == GDT_Byte;

    int targetWidth = 0, targetHeight = 0;
    computeThumbTargetSize(frameWidth, frameHeight, thumbSize, targetWidth, targetHeight,
                           pixelAspectOf(datasetHandle(src)));
    // Only non-Byte data reads the statistics of its stretch (below)
    requireThumbnailBlocks(datasetHandle(src), targetWidth, targetHeight, !hasPalette && !isByte, openPath);

    // Grey (and grey + alpha) is replicated to RGB; otherwise the first three bands.
    // Only non-Byte data (DEM, thermal...) is stretched over the minimum to the maximum of each band,
    // from the statistics of the stretch (the shown bands read in one pass, a sample of a large raster:
    // the worker that renders a thumbnail runs under a cpu time limit): 8-bit photos keep their colours.
    const std::vector<int> bands = bandCount <= 2 ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
    std::vector<ScaleRange> scales;
    if (!hasPalette && !isByte) {
        for (const auto& s : RasterStatsProvider::instance().getStretchStatistics(io::toUtf8(imagePath), hSrc, bands))
            scales.push_back({s.min, s.max});
    }

    const auto alpha = alphaSourceOf(hSrc, bandCount);
    RasterImage image = translateToImage(hSrc, {targetWidth, targetHeight, bands, hasPalette, scales, openPath, alpha});
    if (hasPalette && alpha)
        readPaletteAlpha(hSrc, *alpha, image);

    LOGD << "Rendered " << (image.hasAlpha() ? "RGBA" : "RGB") << " thumbnail of " << openPath;
    return image;
}

// The temperature of a grid pixel as the thumbnail shows it: whether a sensor can see it (not NaN, above absolute
// zero, below 1000 degrees Celsius: the rule of the value endpoints, thermalGridHasValue)
bool isGridTemperature(float t) {
    return std::isfinite(t) && t > -273.15f && t < 1000.0f;
}

// The value of the grid at the fractional pixel position (gx, gy), pixel centres at +0.5: bilinear over the
// neighbours that have a temperature, NODATA_SENTINEL when none has
float sampleGrid(const std::vector<float>& grid, int width, int height, double gx, double gy) {
    const double fx = std::clamp(gx - 0.5, 0.0, static_cast<double>(width - 1));
    const double fy = std::clamp(gy - 0.5, 0.0, static_cast<double>(height - 1));
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double wx = fx - x0;
    const double wy = fy - y0;

    double sum = 0, weights = 0;
    const auto add = [&](int x, int y, double w) {
        const float v = grid[static_cast<size_t>(y) * width + x];
        if (w <= 0 || v == NODATA_SENTINEL) return;
        sum += v * w;
        weights += w;
    };
    add(x0, y0, (1 - wx) * (1 - wy));
    add(x1, y0, wx * (1 - wy));
    add(x0, y1, (1 - wx) * wy);
    add(x1, y1, wx * wy);
    return weights > 0 ? static_cast<float>(sum / weights) : NODATA_SENTINEL;
}

// The thumbnail of an R-JPEG the visualisation parameters colour (FR-RAS-15): its temperatures, from the raw
// thermal data of the camera and not from the colour preview GDAL reads (FR-RAS-14), in the unit of the thermal
// formula (CELSIUS when the parameters name none), over the rescale or else the minimum to the maximum of the image:
// the coldest pixel has the first colour of the colormap and the hottest the last. An R-JPEG whose raw counts no
// conversion turns into temperatures (DJI, whose conversion is DJI's own) is coloured by its raw counts, from their
// minimum to their maximum: a thermal formula gives them no unit, and the rescale applies only without one (with a
// formula it is in a unit of temperature). Nothing when the parameters do not ask for a thermal rendering, or the file
// is no R-JPEG (the caller renders it as any raster).
std::optional<RasterImage> renderRJpegThumbnail(const fs::path& imagePath, int thumbSize, const ThumbVisParams& vis) {
    if (!vis.formula.empty() && !isThermalFormulaId(vis.formula)) return std::nullopt;
    if (vis.formula.empty() && vis.colormap.empty()) return std::nullopt;

    std::optional<RJpegThermal> rjpeg;
    try {
        rjpeg = readRJpegThermal(io::toUtf8(imagePath));
    } catch (const std::exception& e) {
        // A FLIR picture needs the tags of the file for its calibration, which the raster helper cannot read
        LOGD << "No thermal rendering of " << io::toUtf8(imagePath) << ": " << e.what();
        return std::nullopt;
    }
    if (!rjpeg) return std::nullopt;

    auto& ve = VegetationEngine::instance();
    const std::string colormapId = vis.colormap.empty() ? "ironbow" : vis.colormap;
    const Colormap* colormap = ve.getColormap(colormapId);
    if (!colormap) throw InvalidArgsException("Unknown colormap: " + colormapId);

    // The grid in the unit of the formula (or the raw counts without a unit), NODATA_SENTINEL where no temperature
    // can be told
    const int width = rjpeg->grid.width;
    const int height = rjpeg->grid.height;
    const bool rawCounts = !rjpeg->hasTemperatures();
    std::vector<float> grid(static_cast<size_t>(width) * height);
    float vMin = std::numeric_limits<float>::max();
    float vMax = std::numeric_limits<float>::lowest();
    if (rawCounts) {
        for (size_t i = 0; i < grid.size(); i++) {
            grid[i] = static_cast<float>(rjpeg->raw.data[i]);
            vMin = std::min(vMin, grid[i]);
            vMax = std::max(vMax, grid[i]);
        }
    } else {
        const ThermalFormulaConversion unit = celsiusFormulaConversion(vis.formula.empty() ? "CELSIUS" : vis.formula);
        for (size_t i = 0; i < grid.size(); i++) {
            const float t = rjpeg->grid.temperatures[i];
            if (!isGridTemperature(t)) {
                grid[i] = NODATA_SENTINEL;
                continue;
            }
            grid[i] = static_cast<float>(unit.conversion.apply(t));
            vMin = std::min(vMin, grid[i]);
            vMax = std::max(vMax, grid[i]);
        }
    }
    if (vMin > vMax) return std::nullopt;

    std::pair<float, float> range = {vMin, vMax > vMin ? vMax : vMin + 1.0f};
    if (!vis.rescale.empty() && !(rawCounts && !vis.formula.empty())) range = parseRescaleRange(vis.rescale);

    int targetWidth = 0, targetHeight = 0;
    computeThumbTargetSize(width, height, thumbSize, targetWidth, targetHeight);
    std::vector<float> values(static_cast<size_t>(targetWidth) * targetHeight);
    for (int y = 0; y < targetHeight; y++) {
        for (int x = 0; x < targetWidth; x++) {
            values[static_cast<size_t>(y) * targetWidth + x] =
                sampleGrid(grid, width, height, (x + 0.5) * width / targetWidth, (y + 0.5) * height / targetHeight);
        }
    }

    std::vector<uint8_t> rgba(values.size() * 4);
    ve.applyColormap(values.data(), rgba.data(), values.size(), *colormap, {range.first, range.second, NODATA_SENTINEL});

    RasterImage image(targetWidth, targetHeight, 3, true);
    for (size_t i = 0; i < image.pixels(); i++) {
        for (int b = 0; b < 3; b++) image.band(b)[i] = rgba[i * 4 + b];
        image.alpha[i] = rgba[i * 4 + 3];
    }
    LOGD << "Rendered the temperatures of the R-JPEG " << io::toUtf8(imagePath) << " as a thumbnail";
    return image;
}

// The image of RGBA interleaved pixels of width x height
RasterImage rgbaImage(const std::vector<uint8_t>& rgba, int width, int height) {
    RasterImage image(width, height, 3, true);
    for (size_t i = 0; i < image.pixels(); i++) {
        for (int b = 0; b < 3; b++) image.band(b)[i] = rgba[i * 4 + b];
        image.alpha[i] = rgba[i * 4 + 3];
    }
    return image;
}

// The thumbnail the visualisation parameters describe: a formula with its colormap, one band with a
// colormap, or the bands they choose with a stretch. thermalTags is what the host found in the tags
// of the file: the helper reads none.
RasterImage renderVisualisedThumbnail(const fs::path& imagePath, int thumbSize, const ThumbVisParams& visParams,
                                      bool thermalTags) {
    // An R-JPEG shows its temperatures, not the colours of its preview
    if (auto rjpeg = renderRJpegThumbnail(imagePath, thumbSize, visParams))
        return std::move(*rjpeg);

    const auto src = openUserRaster(io::toUtf8(imagePath));
    if (!src)
        throw raster::RasterOpException(raster::ErrorCode::Unreadable, "Cannot open " + io::toUtf8(imagePath) + " for reading");
    // With a NoData override the thumbnail reads a view of the raster that has it: the formula, the mask
    // and the statistics of the stretch then all see the same pixels as without data
    GDALDatasetUniquePtr overridden;
    if (visParams.nodata)
        overridden = withNodataOverride(datasetHandle(src), *visParams.nodata);
    GDALDatasetH hSrc = overridden ? datasetHandle(overridden) : datasetHandle(src);

    const int width = GDALGetRasterXSize(hSrc);
    const int height = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    const GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrc, 1));

    int targetWidth, targetHeight;
    computeThumbTargetSize(width, height, thumbSize, targetWidth, targetHeight, pixelAspectOf(hSrc));
    // A formula or a colormap reads the range of its values, non-Byte data without a rescale the
    // statistics of its stretch (below)
    const bool sampled = !visParams.formula.empty() || !visParams.colormap.empty() ||
                         (srcType != GDT_Byte && visParams.rescale.empty());
    requireThumbnailBlocks(datasetHandle(src), targetWidth, targetHeight, sampled, io::toUtf8(imagePath));

    // Formula mode: vegetation index rendered with its colormap
    if (!visParams.formula.empty()) {
        const RasterWindow window{0, 0, width, height, targetWidth, targetHeight};
        return rgbaImage(renderFormulaRgba(hSrc, io::toUtf8(imagePath), window, visParams, thermalTags), targetWidth,
                         targetHeight);
    }

    // One band with a colormap and no formula (FR-RAS-01): the picture the colormap makes of the band, as the map
    // tiles and the export draw it, stretched as the other thumbnails over the statistics of a bounded read (D-041:
    // the exact ones when they are known, else a sample of a large raster)
    if (const auto style =
            planVisBandStyle(io::toUtf8(imagePath), hSrc, visParams, bandCount, StretchSource::Bounded)) {
        const RasterWindow window{0, 0, width, height, targetWidth, targetHeight};
        return rgbaImage(renderBandStyleRgba(hSrc, *style, window), targetWidth, targetHeight);
    }

    // Band selection (explicit, preset or detected sensor) + stretch
    std::vector<int> bands = resolveVisBands(io::toUtf8(imagePath), visParams, bandCount);
    if (bands.empty())
        bands = bandCount <= 2 ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
    bands.resize(3, bands.back());

    // Explicit range for all bands; for non-Byte data otherwise the common p2-p98
    // (approximated from the statistics) of the shown bands. 8-bit data is not stretched.
    std::vector<ScaleRange> scales;
    if (!visParams.rescale.empty()) {
        const auto [rMin, rMax] = parseRescaleRange(visParams.rescale);
        scales.assign(bands.size(), {rMin, rMax});
    } else if (srcType != GDT_Byte) {
        double globalMin = std::numeric_limits<double>::max();
        double globalMax = std::numeric_limits<double>::lowest();
        // The shown bands in one pass, a sample of a large raster (getStretchStatistics)
        for (const auto& s :
             RasterStatsProvider::instance().getStretchStatistics(io::toUtf8(imagePath), hSrc, bands, visParams.nodata)) {
            const auto [sMin, sMax] = stretchRange(s);
            globalMin = std::min(globalMin, sMin);
            globalMax = std::max(globalMax, sMax);
        }
        scales.assign(bands.size(), {globalMin, globalMax});
    }

    return translateToImage(hSrc, {targetWidth, targetHeight, bands, false, scales, io::toUtf8(imagePath),
                                   alphaSourceOf(hSrc, bandCount)});
}

}  // namespace

namespace raster {

RasterImage renderThumbnail(const fs::path& imagePath, const ThumbnailParams& params) {
    return params.vis ? renderVisualisedThumbnail(imagePath, params.size, *params.vis, params.thermalTags)
                      : renderPlainThumbnail(imagePath, params.size);
}

void thumbnail(const fs::path& imagePath, const fs::path& outFile, const ThumbnailParams& params) {
    thumb::write(renderThumbnail(imagePath, params), outFile, nullptr, nullptr);
}

}  // namespace raster

}  // namespace ddb
