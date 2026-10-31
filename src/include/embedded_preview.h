/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EMBEDDED_PREVIEW_H
#define EMBEDDED_PREVIEW_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fs.h"

namespace ddb
{
    /// A JPEG image stored inside another JPEG file (offset and size in the file)
    struct JpegPreview
    {
        uint64_t offset = 0;
        uint64_t size = 0;
        int width = 0;
        int height = 0;
    };

    /// Size of a JPEG file's main image and the further images of its MPF index
    /// (CIPA DC-007, written by DJI and most phones: e.g. a 960x720 copy of a 4000x3000 photo)
    struct JpegLayout
    {
        int width = 0;
        int height = 0;
        std::vector<JpegPreview> previews;
    };

    /// Reads the markers of a JPEG file up to its first scan. A preview is listed only when
    /// its bytes lie inside the file and start a JPEG image with a frame header (files
    /// re-saved by an editor often keep an MPF index that points past their end).
    /// Throws FSException when the file cannot be read; returns an empty layout (size 0)
    /// when it is not a JPEG.
    JpegLayout readJpegLayout(const fs::path &path);

    /// The smallest preview with the proportions of the main image (1 % tolerance) whose
    /// long side is at least minLongSide; nothing when there is none
    std::optional<JpegPreview> pickJpegPreview(const JpegLayout &layout, int minLongSide);

    /// GDAL path that opens the preview inside the file (/vsisubfile/)
    std::string jpegPreviewGdalPath(const fs::path &path, const JpegPreview &preview);
}

#endif // EMBEDDED_PREVIEW_H
