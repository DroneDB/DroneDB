/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <cstddef>

#include "mio.h"
#include "thumb_internal.h"
#include "tilers_call.h"

namespace ddb {

void generatePointCloudThumb(const fs::path& copcPath,
                             int thumbSize,
                             const fs::path& outImagePath,
                             uint8_t** outBuffer,
                             int* outBufferSize,
                             uint64_t pointBudget) {
    // ddb-tilers draws the whole cloud as its tiles draw it (local, or at a URL through libddb's
    // HTTP client), into RGBA
    uint8_t* raw = nullptr;
    int size = 0;
    tilers::check(ddbtRenderCopcThumbnail(io::toUtf8(copcPath).c_str(), thumbSize, pointBudget, &raw, &size));
    const tilers::Bytes rgba(raw);

    RasterImage image(thumbSize, thumbSize, 3, true);
    const size_t pixels = image.pixels();
    for (size_t i = 0; i < pixels; i++) {
        for (int band = 0; band < 3; band++)
            image.band(band)[i] = rgba.get()[i * 4 + static_cast<size_t>(band)];
        image.alpha[i] = rgba.get()[i * 4 + 3];
    }
    thumb::write(image, outImagePath, outBuffer, outBufferSize);
}

}  // namespace ddb
