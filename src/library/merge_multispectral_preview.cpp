/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The preview half of a multispectral merge: three bands of the stack as a small WebP.

#include "exceptions.h"
#include "gdal_raii.h"
#include "merge_multispectral.h"
#include "merge_vrt.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "temp_names.h"

namespace ddb {

namespace raster {

void mergePreviewFile(const MergePreviewParams &params, const fs::path &out) {
    std::vector<std::string> inputs;
    for (const fs::path &input : params.inputs) inputs.push_back(io::toUtf8(input));

    // Build VRT with separate=TRUE
    const merge::SeparateVrt merged = merge::buildSeparateVrt(merge::openAll(inputs), {}, ".vrt", "Cannot build VRT");

    // Translate with band selection and scaling
    CPLStringList tArgs;
    tArgs.AddString("-of");
    tArgs.AddString("WEBP");
    tArgs.AddString("-outsize");
    tArgs.AddString(std::to_string(params.size).c_str());
    tArgs.AddString(std::to_string(params.size).c_str());
    tArgs.AddString("-ot");
    tArgs.AddString("Byte");
    tArgs.AddString("-scale");

    for (int band : params.bands) {
        tArgs.AddString("-b");
        tArgs.AddString(std::to_string(band).c_str());
    }

    const GdalOptionsPtr<GDALTranslateOptions> transOpts(GDALTranslateOptionsNew(tArgs.List(), nullptr));

    auto result = adoptDataset(GDALTranslate(io::toUtf8(out).c_str(), merged.handle(), transOpts.get(), nullptr));
    if (!result)
        throw GDALException("Cannot generate preview");

    closeOutputOrThrow(std::move(result), "the preview");
}

}  // namespace raster

void previewMergeMultispectral(const std::vector<std::string> &inputPaths,
                                const std::vector<int> &previewBands,
                                int thumbSize,
                                uint8_t **outBuffer, int *outBufferSize) {
    if (inputPaths.empty()) throw InvalidArgsException("No input paths");
    if (previewBands.size() < 3) throw InvalidArgsException("Need at least 3 preview bands");

    raster::MergePreviewParams params;
    params.inputs = io::fromUtf8(inputPaths);
    params.bands = previewBands;
    params.size = thumbSize;

    // The raster helper reads the files (a user's) and writes the preview in a private folder
    const io::ScopedTempDir request = raster::makePrivateFolder(fs::temp_directory_path(), tempnames::kMergePreview);
    const fs::path output = request.path() / "preview.webp";
    raster::rasterHelper().operate(raster::op::kMergePreview, params.inputs.front(), output, raster::toJson(params),
                                   raster::ProgressCallback());

    const std::string webp = raster::readHelperWebp(output, inputPaths.front());
    *outBuffer = vsiMallocCopy(webp);
    *outBufferSize = static_cast<int>(webp.size());
}

}  // namespace ddb
