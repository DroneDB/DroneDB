/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "image_translation.h"

#include <algorithm>
#include <cstddef>

#include "cancellation.h"
#include "exceptions.h"
#include "gdal_raii.h"

namespace ddb::imaging {

namespace {

// Keeps the alpha of the image only when some pixel is not opaque
void keepAlphaIfTranslucent(std::vector<uint8_t> alpha, RasterImage& image) {
    if (std::any_of(alpha.begin(), alpha.end(), [](uint8_t v) { return v < 255; }))
        image.alpha = std::move(alpha);
}

}  // namespace

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

RasterImage translateToImage(GDALDatasetH hSrc, const ImageTranslation& spec) {
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
                                         utilityNumber(spec.scales[i].min), utilityNumber(spec.scales[i].max), "0", "255"});
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

}  // namespace ddb::imaging
