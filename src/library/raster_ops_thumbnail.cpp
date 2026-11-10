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
#include "logger.h"
#include "mio.h"
#include "raster_stats.h"
#include "raster_utils.h"
#include "thumbs.h"
#include "thermal.h"
#include "thumb_internal.h"
#include "vis_render.h"

namespace ddb {

namespace {

// Range of source values mapped to 0-255 for one output band
struct ScaleRange {
    double min;
    double max;
};

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

// Compute thumbnail target dimensions preserving aspect ratio.
// Guaranteed to return values >= 1 pixel.
void computeThumbTargetSize(int srcWidth, int srcHeight, int thumbSize,
                            int& outWidth, int& outHeight) {
    if (srcWidth <= 0 || srcHeight <= 0 || thumbSize <= 0)
        throw InvalidArgsException("Invalid thumbnail size parameters");

    if (srcWidth > srcHeight) {
        outWidth = thumbSize;
        outHeight = static_cast<int>((static_cast<float>(thumbSize) /
                                      static_cast<float>(srcWidth)) *
                                     static_cast<float>(srcHeight));
    } else {
        outHeight = thumbSize;
        outWidth = static_cast<int>((static_cast<float>(thumbSize) /
                                     static_cast<float>(srcHeight)) *
                                    static_cast<float>(srcWidth));
    }
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;
}

// The band the alpha of a thumbnail comes from: a band of the source, or the mask of one
struct AlphaSource {
    int band = 1;
    bool mask = false;

    // As gdal_translate names it (-b)
    std::string translateBand() const { return (mask ? "mask," : "") + std::to_string(band); }

    GDALRasterBandH handle(GDALDatasetH hSrc) const {
        GDALRasterBandH hBand = GDALGetRasterBand(hSrc, band);
        return mask && hBand ? GDALGetMaskBand(hBand) : hBand;
    }
};

// The alpha of the thumbnail: the second band of a 2-band raster (grey and alpha), the alpha band
// of a raster of 4 bands or more, otherwise the mask of the first band. Nothing when the source is
// known to be fully opaque (no nodata, no alpha band, no mask), so the thumbnail has no alpha.
std::optional<AlphaSource> alphaSourceOf(GDALDatasetH hSrc, int bandCount) {
    // Generic 2-band rasters conventionally carry grayscale + alpha.
    if (bandCount == 2)
        return AlphaSource{2, false};

    // Prefer an explicit alpha band when present on 4+ band rasters.
    if (bandCount >= 4) {
        GDALRasterBandH band4 = GDALGetRasterBand(hSrc, 4);
        if (band4 && GDALGetRasterColorInterpretation(band4) == GCI_AlphaBand)
            return AlphaSource{4, false};
    }

    GDALRasterBandH band1 = GDALGetRasterBand(hSrc, 1);
    if (!band1)
        return std::nullopt;

    // GDALGetMaskBand always returns a valid handle, but for fully-valid
    // rasters its only flag is GMF_ALL_VALID. In that case skip the extra
    // mask read and output a plain 3-band WEBP.
    if (GDALGetMaskFlags(band1) == GMF_ALL_VALID)
        return std::nullopt;

    return AlphaSource{1, true};
}

// Keeps the alpha of the image only when some pixel is not opaque
void keepAlphaIfTranslucent(std::vector<uint8_t> alpha, RasterImage& image) {
    if (std::any_of(alpha.begin(), alpha.end(), [](uint8_t v) { return v < 255; }))
        image.alpha = std::move(alpha);
}

// Reads the alpha of a palette image at target resolution, averaged, into image.alpha. Leaves the
// image without alpha when every pixel is opaque.
void readPaletteAlpha(GDALDatasetH hSrc, const AlphaSource& alpha, RasterImage& image) {
    std::vector<uint8_t> buffer(image.pixels());

    // A full-raster read when the band has no overviews: it stops when the call is canceled
    cancel::GdalProgress progress;
    GDALRasterIOExtraArg extraArg = progress.rasterIOExtraArg(GRIORA_Average);

    if (GDALRasterIOEx(alpha.handle(hSrc), GF_Read, 0, 0, GDALGetRasterXSize(hSrc), GDALGetRasterYSize(hSrc),
                       buffer.data(), image.width, image.height,
                       GDT_Byte, 0, 0, &extraArg) != CE_None) {
        cancel::throwIfCanceled();
        throw GDALException(std::string("Failed to read alpha/mask band: ") +
                            CPLGetLastErrorMsg());
    }
    keepAlphaIfTranslucent(std::move(buffer), image);
}

// How a source is resampled into the 3-band Byte image of a thumbnail
struct ImageTranslation {
    int width = 0;
    int height = 0;
    // bands[i] is the source band of output band i (ignored when expandPalette)
    std::vector<int> bands;
    bool expandPalette = false;
    // scales[i], when given, maps [min, max] of output band i to 0-255; Byte data without
    // scales is copied as is
    std::vector<ScaleRange> scales;
    std::string sourceLabel;
    // The band of the alpha, resampled with the colours (ignored when expandPalette: a palette is
    // expanded by its nearest pixels, its alpha averaged, so it is read on its own)
    std::optional<AlphaSource> alpha;
};

// Resamples the source to a 3-band Byte image of the requested size, with its alpha when the
// spec names one and some pixel is not opaque
RasterImage translateToImage(GDALDatasetH hSrc, const ImageTranslation& spec) {
    const auto fmt = [](double v) {
        std::ostringstream os;
        os << std::setprecision(17) << v;
        return os.str();
    };

    // Without overviews every pixel of the raster is read. GDALTranslate reads its source through
    // a VRT whose blocks are 128 x 128 pixels of the output, every band of a block before the next
    // block: one block covers thousands of rows of the source, and a pixel-interleaved file decodes
    // all its bands for any of them, so when that window does not fit in the block cache the file
    // is decoded again for each band read (four times for a raster of 52 bands, a thumbnail longer
    // than the helper allows). Blocks of one output row cover a few rows of the source: the first
    // band decodes them and the next ones find them in the cache, so the file is decoded once.
    // Such a VRT is read in place of a copy. A raster with overviews is read from them in the
    // windows it always had: rows would place the pixels of an overview otherwise. The alpha is a
    // band of the same translation: a read of its own would decode the whole file once more (its
    // mask is the nodata of the first band), which doubled the time of such a thumbnail.
    const bool fullResolution = GDALGetOverviewCount(GDALGetRasterBand(hSrc, 1)) == 0;
    const bool alpha = spec.alpha && !spec.expandPalette;

    std::vector<std::string> argStore = {
        "-outsize", std::to_string(spec.width), std::to_string(spec.height),
        "-ot", "Byte",
        "-r", spec.expandPalette ? "nearest" : "average",
        "-of", fullResolution ? "VRT" : "MEM",
        // Drop SRS: the thumbnail is meant as a display-only artifact.
        "-a_srs", ""};
    if (fullResolution)
        argStore.insert(argStore.end(), {"-co", "BLOCKXSIZE=" + std::to_string(spec.width), "-co", "BLOCKYSIZE=1"});

    if (spec.expandPalette) {
        // Palette transparency, if any, is recovered from the source mask band.
        argStore.insert(argStore.end(), {"-expand", "rgb"});
    } else {
        for (int b : spec.bands)
            argStore.insert(argStore.end(), {"-b", std::to_string(b)});
        if (alpha)
            argStore.insert(argStore.end(), {"-b", spec.alpha->translateBand()});
    }

    for (size_t i = 0; i < spec.scales.size(); i++) {
        argStore.insert(argStore.end(), {"-scale_" + std::to_string(i + 1),
                                         fmt(spec.scales[i].min), fmt(spec.scales[i].max), "0", "255"});
    }

    CPLStringList argv = cslFromStrings(argStore);
    const GdalOptionsPtr<GDALTranslateOptions> psOptions(
        GDALTranslateOptionsNew(argv.List(), nullptr));
    if (!psOptions)
        throw GDALException("Failed to create GDAL translate options for " + spec.sourceLabel);
    // The resampling reads the whole raster (in the copy, or in the read of the VRT): it stops
    // when the call is canceled
    cancel::GdalProgress progress;
    progress.attach(psOptions.get());

    const auto translated = adoptDataset(GDALTranslate("", hSrc, psOptions.get(), nullptr));
    if (!translated) {
        cancel::throwIfCanceled();
        throw GDALException("Failed to normalize " + spec.sourceLabel + " for thumbnail: " + CPLGetLastErrorMsg());
    }

    // The colours and the alpha in one read, so the source is decoded once for all of them: the
    // alpha is read as a fourth plane after the colours and then moved out of them
    RasterImage image(spec.width, spec.height, 3, false);
    const int readBands = alpha ? 4 : 3;
    image.color.resize(image.pixels() * static_cast<size_t>(readBands));
    GDALRasterIOExtraArg extraArg = progress.rasterIOExtraArg();
    if (GDALDatasetRasterIOEx(datasetHandle(translated), GF_Read, 0, 0, spec.width, spec.height, image.color.data(),
                              spec.width, spec.height, GDT_Byte, readBands, nullptr, 0, 0, 0, &extraArg) != CE_None) {
        cancel::throwIfCanceled();
        throw GDALException(std::string("Failed to read thumbnail pixels: ") + CPLGetLastErrorMsg());
    }
    if (alpha) {
        const auto colourEnd = image.color.begin() + static_cast<std::ptrdiff_t>(image.pixels() * 3);
        std::vector<uint8_t> alphaPixels(colourEnd, image.color.end());
        image.color.erase(colourEnd, image.color.end());
        keepAlphaIfTranslucent(std::move(alphaPixels), image);
    }
    return image;
}

// The thumbnail of a photo or a raster: its colours, or the first bands stretched to 0-255 for
// data that is not 8-bit
RasterImage renderPlainThumbnail(const fs::path& imagePath, int thumbSize) {
    std::string openPath;
    int frameWidth = 0, frameHeight = 0;
    const auto src = openImageThumbSource(imagePath, thumbSize, openPath, frameWidth, frameHeight);
    GDALDatasetH hSrc = datasetHandle(src);

    const int srcWidth = GDALGetRasterXSize(hSrc);
    const int srcHeight = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    if (srcWidth <= 0 || srcHeight <= 0 || bandCount <= 0 || frameWidth <= 0 || frameHeight <= 0)
        throw GDALException("Invalid source raster dimensions in " + openPath);

    GDALRasterBandH hBand1 = GDALGetRasterBand(hSrc, 1);
    const bool hasPalette = hBand1 && GDALGetRasterColorTable(hBand1) != nullptr;
    const bool isByte = GDALGetRasterDataType(hBand1) == GDT_Byte;

    int targetWidth = 0, targetHeight = 0;
    computeThumbTargetSize(frameWidth, frameHeight, thumbSize, targetWidth, targetHeight);

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
// the coldest pixel has the first colour of the colormap and the hottest the last. Nothing when the parameters do not
// ask for a thermal rendering, or the file is no R-JPEG with temperatures (the caller renders it as any raster).
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
    if (!rjpeg || rjpeg->grid.temperatures.empty()) return std::nullopt;

    auto& ve = VegetationEngine::instance();
    const std::string colormapId = vis.colormap.empty() ? "ironbow" : vis.colormap;
    const Colormap* colormap = ve.getColormap(colormapId);
    if (!colormap) throw InvalidArgsException("Unknown colormap: " + colormapId);
    const ThermalFormulaConversion unit = celsiusFormulaConversion(vis.formula.empty() ? "CELSIUS" : vis.formula);

    // The grid in the unit of the formula, NODATA_SENTINEL where no temperature can be told
    const int width = rjpeg->grid.width;
    const int height = rjpeg->grid.height;
    std::vector<float> grid(rjpeg->grid.temperatures.size());
    float vMin = std::numeric_limits<float>::max();
    float vMax = std::numeric_limits<float>::lowest();
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
    if (vMin > vMax) return std::nullopt;

    std::pair<float, float> range = {vMin, vMax > vMin ? vMax : vMin + 1.0f};
    if (!vis.rescale.empty()) range = parseRescaleRange(vis.rescale);

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

// The thumbnail the visualisation parameters describe: a formula with its colormap, or the bands
// they choose with a stretch. thermalTags is what the host found in the tags of the file: the
// helper reads none.
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
    computeThumbTargetSize(width, height, thumbSize, targetWidth, targetHeight);

    // Formula mode: vegetation index rendered with its colormap
    if (!visParams.formula.empty()) {
        const RasterWindow window{0, 0, width, height, targetWidth, targetHeight};
        const auto rgba = renderFormulaRgba(hSrc, io::toUtf8(imagePath), window, visParams, thermalTags);

        RasterImage image(targetWidth, targetHeight, 3, true);
        for (size_t i = 0; i < image.pixels(); i++) {
            for (int b = 0; b < 3; b++) image.band(b)[i] = rgba[i * 4 + b];
            image.alpha[i] = rgba[i * 4 + 3];
        }
        return image;
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
