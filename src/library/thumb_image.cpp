/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

#include "cancellation.h"
#include "embedded_preview.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "logger.h"
#include "mio.h"
#include "raster_stats.h"
#include "thumb_internal.h"
#include "vis_render.h"

namespace ddb {

namespace {

// Range of source values mapped to 0-255 for one output band
struct ScaleRange {
    double min;
    double max;
};

GDALDatasetUniquePtr openThumbSource(const fs::path& imagePath, std::string& resolvedPath) {
    resolvedPath = io::toUtf8(imagePath);
    bool tryReopen = false;
    if (io::isNetworkPath(resolvedPath) &&
        io::Path(io::fromUtf8(resolvedPath)).checkExtension({"tif", "tiff"})) {
        resolvedPath = "/vsicurl/" + resolvedPath;
        tryReopen = true;
    }

    auto src = adoptDataset(GDALOpen(resolvedPath.c_str(), GA_ReadOnly));
    if (!src && tryReopen) {
        resolvedPath = io::toUtf8(imagePath);
        src = adoptDataset(GDALOpen(resolvedPath.c_str(), GA_ReadOnly));
    }
    if (!src)
        throw GDALException("Cannot open " + resolvedPath + " for reading");
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
    if (!io::isNetworkPath(io::toUtf8(imagePath)) && io::Path(imagePath).checkExtension({"jpg", "jpeg"})) {
        const JpegLayout layout = readJpegLayout(imagePath);
        if (const auto preview = pickJpegPreview(layout, thumbSize)) {
            openPath = jpegPreviewGdalPath(imagePath, *preview);
            if (auto previewDs = adoptDataset(GDALOpen(openPath.c_str(), GA_ReadOnly))) {
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

// Decide which source band should provide the alpha channel for the thumbnail.
// Returns nullptr when the source is known to be fully opaque (no nodata, no
// alpha band, no per-dataset mask) so the caller can skip the alpha pass.
GDALRasterBandH resolveAlphaBand(GDALDatasetH hSrc, int bandCount) {
    // Generic 2-band rasters conventionally carry grayscale + alpha.
    if (bandCount == 2)
        return GDALGetRasterBand(hSrc, 2);

    // Prefer an explicit alpha band when present on 4+ band rasters.
    if (bandCount >= 4) {
        GDALRasterBandH band4 = GDALGetRasterBand(hSrc, 4);
        if (band4 && GDALGetRasterColorInterpretation(band4) == GCI_AlphaBand)
            return band4;
    }

    GDALRasterBandH band1 = GDALGetRasterBand(hSrc, 1);
    if (!band1)
        return nullptr;

    // GDALGetMaskBand always returns a valid handle, but for fully-valid
    // rasters its only flag is GMF_ALL_VALID. In that case skip the extra
    // mask read and output a plain 3-band WEBP.
    const int maskFlags = GDALGetMaskFlags(band1);
    if (maskFlags == GMF_ALL_VALID)
        return nullptr;

    return GDALGetMaskBand(band1);
}

// Read an alpha source band at target resolution into image.alpha. Leaves the image
// without alpha when every pixel is opaque.
void readThumbAlpha(GDALRasterBandH hAlphaBand, int srcWidth, int srcHeight, RasterImage& image) {
    std::vector<uint8_t> buffer(image.pixels());

    // A full-raster read when the band has no overviews: it stops when the call is canceled
    cancel::GdalProgress progress;
    GDALRasterIOExtraArg extraArg = progress.rasterIOExtraArg(GRIORA_Average);

    if (GDALRasterIOEx(hAlphaBand, GF_Read, 0, 0, srcWidth, srcHeight,
                       buffer.data(), image.width, image.height,
                       GDT_Byte, 0, 0, &extraArg) != CE_None) {
        cancel::throwIfCanceled();
        throw GDALException(std::string("Failed to read alpha/mask band: ") +
                            CPLGetLastErrorMsg());
    }

    if (std::any_of(buffer.begin(), buffer.end(), [](uint8_t v) { return v < 255; }))
        image.alpha = std::move(buffer);
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
};

// Resamples the source to a 3-band Byte image of the requested size
RasterImage translateToImage(GDALDatasetH hSrc, const ImageTranslation& spec) {
    const auto fmt = [](double v) {
        std::ostringstream os;
        os << std::setprecision(17) << v;
        return os.str();
    };

    std::vector<std::string> argStore = {
        "-outsize", std::to_string(spec.width), std::to_string(spec.height),
        "-ot", "Byte",
        "-r", spec.expandPalette ? "nearest" : "average",
        "-of", "MEM",
        // Drop SRS: the thumbnail is meant as a display-only artifact.
        "-a_srs", ""};

    if (spec.expandPalette) {
        // Palette transparency, if any, is recovered from the source mask band.
        argStore.insert(argStore.end(), {"-expand", "rgb"});
    } else {
        for (int b : spec.bands)
            argStore.insert(argStore.end(), {"-b", std::to_string(b)});
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
    // The resampling reads the whole raster: it stops when the call is canceled
    cancel::GdalProgress progress;
    progress.attach(psOptions.get());

    const auto translated = adoptDataset(GDALTranslate("", hSrc, psOptions.get(), nullptr));
    if (!translated) {
        cancel::throwIfCanceled();
        throw GDALException("Failed to normalize " + spec.sourceLabel + " for thumbnail: " + CPLGetLastErrorMsg());
    }

    RasterImage image(spec.width, spec.height, 3, false);
    if (GDALDatasetRasterIO(datasetHandle(translated), GF_Read, 0, 0, spec.width, spec.height, image.color.data(),
                            spec.width, spec.height, GDT_Byte, 3, nullptr, 0, 0, 0) != CE_None)
        throw GDALException(std::string("Failed to read thumbnail pixels: ") + CPLGetLastErrorMsg());
    return image;
}

// Display range of the source band (its minimum and maximum) for a stretch to 0-255
ScaleRange bandRange(const fs::path& imagePath, GDALDatasetH hSrc, int band) {
    const auto s = RasterStatsProvider::instance().get(io::toUtf8(imagePath), hSrc, band);
    return {s.min, s.max};
}

}  // namespace

void generateImageThumb(const fs::path& imagePath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
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
    // Only non-Byte data (DEM, thermal...) is stretched, from the raster statistics:
    // 8-bit photos keep their colours.
    const std::vector<int> bands = bandCount <= 2 ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
    std::vector<ScaleRange> scales;
    if (!hasPalette && !isByte) {
        for (int b : bands)
            scales.push_back(bandRange(imagePath, hSrc, b));
    }

    RasterImage image = translateToImage(hSrc, {targetWidth, targetHeight, bands, hasPalette, scales, openPath});

    if (GDALRasterBandH hAlphaBand = resolveAlphaBand(hSrc, bandCount))
        readThumbAlpha(hAlphaBand, srcWidth, srcHeight, image);

    LOGD << "Writing " << (image.hasAlpha() ? "RGBA" : "RGB") << " WEBP thumbnail for " << openPath;
    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

void generateImageThumbEx(const fs::path& imagePath,
                          int thumbSize,
                          const fs::path& outImagePath,
                          uint8_t** outBuffer,
                          int* outBufferSize,
                          const ThumbVisParams& visParams) {
    const auto src = adoptDataset(GDALOpen(io::toUtf8(imagePath).c_str(), GA_ReadOnly));
    if (!src)
        throw GDALException("Cannot open " + io::toUtf8(imagePath) + " for reading");
    GDALDatasetH hSrc = datasetHandle(src);

    const int width = GDALGetRasterXSize(hSrc);
    const int height = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    const GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrc, 1));

    int targetWidth, targetHeight;
    computeThumbTargetSize(width, height, thumbSize, targetWidth, targetHeight);

    // Formula mode: vegetation index rendered with its colormap
    if (!visParams.formula.empty()) {
        const RasterWindow window{0, 0, width, height, targetWidth, targetHeight};
        const auto rgba = renderFormulaRgba(hSrc, io::toUtf8(imagePath), window, visParams);

        RasterImage image(targetWidth, targetHeight, 3, true);
        for (size_t i = 0; i < image.pixels(); i++) {
            for (int b = 0; b < 3; b++) image.band(b)[i] = rgba[i * 4 + b];
            image.alpha[i] = rgba[i * 4 + 3];
        }
        thumb::write(image, outImagePath, outBuffer, outBufferSize);
        return;
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
        for (int b : bands) {
            const auto [sMin, sMax] = stretchRange(RasterStatsProvider::instance().get(io::toUtf8(imagePath), hSrc, b));
            globalMin = std::min(globalMin, sMin);
            globalMax = std::max(globalMax, sMax);
        }
        scales.assign(bands.size(), {globalMin, globalMax});
    }

    RasterImage image = translateToImage(hSrc, {targetWidth, targetHeight, bands, false, scales, io::toUtf8(imagePath)});
    if (GDALRasterBandH hAlphaBand = resolveAlphaBand(hSrc, bandCount))
        readThumbAlpha(hAlphaBand, width, height, image);

    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

}  // namespace ddb
