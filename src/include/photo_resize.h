/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PHOTO_RESIZE_H
#define PHOTO_RESIZE_H

#include <string>
#include <vector>

#include "fs.h"
#include "raster_protocol.h"

namespace ddb::raster
{
    // The size of each of the user's photos, or why it cannot be read: what the raster helper runs for
    // an `imageSizes` request. Only where users' files are opened (the helper).
    std::vector<PhotoSize> readPhotoSizes(const std::vector<fs::path> &photos);

    // Writes each photo of `params` as a JPEG into `folder` (GDALTranslate -outsize -r average, the EXIF
    // left out; 16-bit samples scaled from the range of their type, 1 or 3 bands kept), a few at a
    // time: what the raster helper runs for a `resizeImages` request. Returns, for each photo, "" when
    // it was written, else why not (its file is removed). `progress` gets the share of the photos done.
    // @throws CanceledException
    std::vector<std::string> resizePhotos(const ResizeImagesParams &params, const fs::path &folder,
                                          const ProgressCallback &progress);
} // namespace ddb::raster

#endif // PHOTO_RESIZE_H
