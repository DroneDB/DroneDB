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

namespace ddb::merge {

// Opens every path; throws GDALException naming the first that cannot be opened
std::vector<GDALDatasetUniquePtr> openAll(const std::vector<std::string> &paths);

// The bands of a merge as one /vsimem/ VRT (-separate) with the sources it reads. The members
// are declared so that the VRT closes first, then its file goes, then the sources and their
// /vsimem/ files (the shifted bands). Movable, not assignable: a member-wise assignment would
// close them in the wrong order; close() does it in the right one.
struct SeparateVrt {
    std::vector<VsiMemFile> sourceFiles;
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
        sourceFiles.clear();
    }
};

// Builds the VRT over the sources (taking them and their files); the VRT file name ends with
// suffix. Throws GDALException(error) when GDAL cannot build it.
SeparateVrt buildSeparateVrt(std::vector<GDALDatasetUniquePtr> sources,
                             std::vector<VsiMemFile> sourceFiles, const std::string &suffix,
                             const std::string &error);

}  // namespace ddb::merge

#endif  // MERGE_VRT_H
