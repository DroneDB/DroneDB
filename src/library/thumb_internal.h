/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THUMB_INTERNAL_H
#define THUMB_INTERNAL_H

// Shared by the thumbnail generators: thumbs.cpp (dispatch and cache), thumb_image.cpp (rasters and
// photos), thumb_pointcloud.cpp (COPC) and thumb_splat.cpp (Gaussian splats). Not installed.

#include <cstdint>

#include "fs.h"
#include "tile_encoder.h"

namespace ddb {

// The thumbnail of a photo or a raster (thumb_image.cpp), written like the other generators
void generateImageThumb(const fs::path& imagePath, int thumbSize, const fs::path& outImagePath,
                        uint8_t** outBuffer, int* outBufferSize);

}  // namespace ddb

namespace ddb::thumb {

// Writes a thumbnail as WEBP to outImagePath, or into *outBuffer when no path is given
void write(const RasterImage& image, const fs::path& outImagePath, uint8_t** outBuffer,
           int* outBufferSize);

}  // namespace ddb::thumb

#endif  // THUMB_INTERNAL_H
