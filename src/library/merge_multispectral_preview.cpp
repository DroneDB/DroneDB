/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The preview half of a multispectral merge: three bands of the stack as a small WebP.

#include "exceptions.h"
#include "gdal_raii.h"
#include "merge_multispectral.h"
#include "merge_vrt.h"

namespace ddb {

void previewMergeMultispectral(const std::vector<std::string> &inputPaths,
                                const std::vector<int> &previewBands,
                                int thumbSize,
                                uint8_t **outBuffer, int *outBufferSize) {
    if (inputPaths.empty()) throw InvalidArgsException("No input paths");
    if (previewBands.size() < 3) throw InvalidArgsException("Need at least 3 preview bands");

    // Build VRT with separate=TRUE
    const merge::SeparateVrt merged =
        merge::buildSeparateVrt(merge::openAll(inputPaths), {}, ".vrt", "Cannot build VRT");

    // Translate with band selection and scaling
    CPLStringList tArgs;
    tArgs.AddString("-of");
    tArgs.AddString("WEBP");
    tArgs.AddString("-outsize");
    tArgs.AddString(std::to_string(thumbSize).c_str());
    tArgs.AddString(std::to_string(thumbSize).c_str());
    tArgs.AddString("-ot");
    tArgs.AddString("Byte");
    tArgs.AddString("-scale");

    for (int band : previewBands) {
        tArgs.AddString("-b");
        tArgs.AddString(std::to_string(band).c_str());
    }

    const GdalOptionsPtr<GDALTranslateOptions> transOpts(
        GDALTranslateOptionsNew(tArgs.List(), nullptr));

    const VsiMemFile outFile = VsiMemFile::unique("", ".webp");
    auto out = adoptDataset(
        GDALTranslate(outFile.c_str(), merged.handle(), transOpts.get(), nullptr));
    if (!out)
        throw GDALException("Cannot generate preview");

    closeOutputOrThrow(std::move(out), "the preview");

    // Read memory buffer
    int bufSize = 0;
    *outBuffer = stealMemFileBuffer(outFile.path(), bufSize).release();
    *outBufferSize = bufSize;
}

}  // namespace ddb
