/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MERGE_CROP_H
#define MERGE_CROP_H

// The plan of the crop of a multispectral merge that shifts the bands into place (AlignmentCrop):
// shared by the validation, which reports the size of the merged raster, and the merge. Not installed.

#include <optional>
#include <vector>

#include "alignment_crop.h"
#include "merge_multispectral.h"

namespace ddb {

// Plans the crop of bands of width x height pixels that the detected alignment asks for; nothing
// when the shift is so large that no area would be left
std::optional<AlignmentCrop> planAlignmentCrop(const std::vector<BandAlignmentInfo> &alignBands,
                                               int width, int height);

}  // namespace ddb

#endif  // MERGE_CROP_H
