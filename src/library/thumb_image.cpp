/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <cstring>

#include "cpl_conv.h"
#include "exceptions.h"
#include "mio.h"
#include "raster_helper.h"
#include "thumb_internal.h"

namespace ddb {

// The GDAL part (reading the file, choosing the bands, the stretch, the encoding) is the
// thumbnail operation of the raster helper (raster_helper.h); this publishes the WebP file it
// made, checked, to a file or into memory, as every generator does.

namespace {

void publish(const std::string& webp,
             const fs::path& outImagePath,
             uint8_t** outBuffer,
             int* outBufferSize) {
    if (outImagePath.empty() && outBuffer != nullptr) {
        if (outBufferSize == nullptr)
            throw InvalidArgsException("Output buffer size pointer is null");
        // The caller frees the buffer with GDAL's allocator, like the buffers of the encoder
        auto* data = static_cast<uint8_t*>(VSIMalloc(webp.size()));
        if (data == nullptr)
            throw AppException("Cannot allocate " + std::to_string(webp.size()) + " bytes");
        std::memcpy(data, webp.data(), webp.size());
        *outBuffer = data;
        *outBufferSize = static_cast<int>(webp.size());
        return;
    }
    // A thumbnail written to a file appears whole or not at all: the thumbnail cache serves any
    // file that exists
    io::writeFileAtomic(outImagePath, webp, "thumbnail");
}

}  // namespace

void generateImageThumb(const fs::path& imagePath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    publish(raster::renderThumbnailWebp(imagePath, {thumbSize, std::nullopt}), outImagePath, outBuffer,
            outBufferSize);
}

void generateImageThumbEx(const fs::path& imagePath,
                          int thumbSize,
                          const fs::path& outImagePath,
                          uint8_t** outBuffer,
                          int* outBufferSize,
                          const ThumbVisParams& visParams) {
    publish(raster::renderThumbnailWebp(imagePath, {thumbSize, visParams}), outImagePath, outBuffer,
            outBufferSize);
}

}  // namespace ddb
