/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MERGE_VRT_H
#define MERGE_VRT_H

// The VRT that stacks single-band rasters as the bands of one raster: shared by the preview and
// the merge of multispectral bands (merge_multispectral_preview.cpp, merge_multispectral.cpp).
// Not installed.

#include <string>
#include <vector>

#include "gdal_raii.h"
#include "merge_crop.h"

namespace ddb::merge {

// Opens every path; throws GDALException naming the first that cannot be opened
std::vector<GDALDatasetUniquePtr> openAll(const std::vector<std::string> &paths);

// The bands of a merge as one /vsimem/ VRT (-separate) with the sources it reads. The members
// are declared so that the VRT closes first, then its file goes, then the sources, then the
// rasters the sources read (the inputs of the shifted bands, each a window of its input). Movable,
// not assignable: a member-wise assignment would close them in the wrong order; close() does it
// in the right one.
struct SeparateVrt {
    std::vector<GDALDatasetUniquePtr> inputs;
    std::vector<GDALDatasetUniquePtr> sources;
    VsiMemFile file;
    GDALDatasetUniquePtr vrt;

    SeparateVrt() = default;
    SeparateVrt(SeparateVrt &&) = default;
    SeparateVrt &operator=(SeparateVrt &&) = delete;

    GDALDatasetH handle() const { return datasetHandle(vrt); }

    void close() {
        vrt.reset();
        file.reset();
        sources.clear();
        inputs.clear();
    }
};

// Builds the VRT over the sources (taking them and the inputs they read); the VRT file name ends
// with suffix. Throws GDALException(error) when GDAL cannot build it.
SeparateVrt buildSeparateVrt(std::vector<GDALDatasetUniquePtr> sources,
                             std::vector<GDALDatasetUniquePtr> inputs, const std::string &suffix,
                             const std::string &error);

// The VRT over the inputs cropped to the area every band covers, each band shifted into place: a
// band is the -srcwin window of its input as a VRT of its own, which reads the input when the
// merge reads it (no pixel is copied). Throws GDALException for an input that cannot be opened or
// a window that does not lie inside its input.
SeparateVrt buildAlignedVrt(const std::vector<std::string> &inputPaths, const AlignmentCrop &crop);

}  // namespace ddb::merge

#endif  // MERGE_VRT_H
