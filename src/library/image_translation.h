/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef IMAGE_TRANSLATION_H
#define IMAGE_TRANSLATION_H

// How the raster helper turns a GDAL dataset into the 8-bit image of a picture it shows (a
// thumbnail, the tile of a photo): which bands, how data that is not 8-bit is stretched, and where
// the alpha comes from. Runs where users' files are opened (the helper). Not installed.

#include <optional>
#include <string>
#include <vector>

#include "gdal_inc.h"
#include "tile_encoder.h"

namespace ddb::imaging {

// Range of source values mapped to 0-255 for one output band
struct ScaleRange {
    double min;
    double max;
};

// The band the alpha of an image comes from: a band of the source, or the mask of one
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

// The alpha of an image of the source: the second band of a 2-band raster (grey and alpha), the
// alpha band of a raster of 4 bands or more, otherwise the mask of the first band. Nothing when the
// source is known to be fully opaque (no nodata, no alpha band, no mask), so the image has no alpha.
std::optional<AlphaSource> alphaSourceOf(GDALDatasetH hSrc, int bandCount);

// Reads the alpha of a palette image at the size of `image`, averaged, into image.alpha. Leaves the
// image without alpha when every pixel is opaque.
void readPaletteAlpha(GDALDatasetH hSrc, const AlphaSource& alpha, RasterImage& image);

// How a source is resampled into the 3-band Byte image
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
// spec names one and some pixel is not opaque. Stops when the call is canceled.
// @throws GDALException when GDAL fails
RasterImage translateToImage(GDALDatasetH hSrc, const ImageTranslation& spec);

}  // namespace ddb::imaging

#endif  // IMAGE_TRANSLATION_H
