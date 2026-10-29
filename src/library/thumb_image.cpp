/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include "raster_ops.h"
#include "thumb_internal.h"

namespace ddb {

// The GDAL part (reading the file, choosing the bands, the stretch) is the thumbnail operation of
// raster_ops.h; this writes its image the way every generator does, to a file or into memory.

void generateImageThumb(const fs::path& imagePath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    const RasterImage image = raster::renderThumbnail(imagePath, {thumbSize, std::nullopt});
    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

void generateImageThumbEx(const fs::path& imagePath,
                          int thumbSize,
                          const fs::path& outImagePath,
                          uint8_t** outBuffer,
                          int* outBufferSize,
                          const ThumbVisParams& visParams) {
    const RasterImage image = raster::renderThumbnail(imagePath, {thumbSize, visParams});
    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

}  // namespace ddb
