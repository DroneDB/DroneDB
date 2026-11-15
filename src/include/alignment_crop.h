/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ALIGNMENT_CROP_H
#define ALIGNMENT_CROP_H

#include <vector>

namespace ddb
{
    // The crop of a multispectral merge that shifts the bands into place: the integer shift of every
    // band against the reference band, and the margins that cropping to the area all bands cover takes
    // off each side. The host plans it from the metadata of the bands; the raster helper applies it.
    struct AlignmentCrop
    {
        std::vector<int> dx, dy;
        int padLeft = 0, padRight = 0, padTop = 0, padBottom = 0;
        int outW = 0, outH = 0;
    };
} // namespace ddb

#endif // ALIGNMENT_CROP_H
