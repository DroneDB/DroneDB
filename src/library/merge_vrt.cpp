/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "merge_vrt.h"

#include "exceptions.h"
#include "artefact_cache.h"
#include "logger.h"

namespace ddb::merge {

std::vector<GDALDatasetUniquePtr> openAll(const std::vector<std::string> &paths) {
    std::vector<GDALDatasetUniquePtr> datasets;
    datasets.reserve(paths.size());
    for (const auto &p : paths) {
        auto ds = raster::openInputRaster(p);
        if (!ds) throw GDALException("Cannot open " + p);
        datasets.push_back(std::move(ds));
    }
    return datasets;
}

SeparateVrt buildSeparateVrt(std::vector<GDALDatasetUniquePtr> sources,
                             std::vector<GDALDatasetUniquePtr> inputs, const std::string &suffix,
                             const std::string &error) {
    SeparateVrt v;
    v.inputs = std::move(inputs);
    v.sources = std::move(sources);

    CPLStringList args;
    args.AddString("-separate");
    args.AddString("-r");
    args.AddString("average");
    const GdalOptionsPtr<GDALBuildVRTOptions> opts(GDALBuildVRTOptionsNew(args.List(), nullptr));

    v.file = VsiMemFile::unique("", suffix);
    auto handles = datasetHandles(v.sources);
    v.vrt = adoptDataset(GDALBuildVRT(v.file.c_str(), static_cast<int>(handles.size()),
                                      handles.data(), nullptr, opts.get(), nullptr));
    if (!v.vrt) throw GDALException(error);
    flushOrThrow(v.handle(), "the VRT of the merge");
    return v;
}

SeparateVrt buildAlignedVrt(const std::vector<std::string> &inputPaths, const AlignmentCrop &crop) {
    // Declared before the windows that read them: closed after them
    std::vector<GDALDatasetUniquePtr> inputs;
    std::vector<GDALDatasetUniquePtr> windows;

    for (size_t i = 0; i < inputPaths.size(); i++) {
        auto input = raster::openInputRaster(inputPaths[i]);
        if (!input) throw GDALException("Cannot open " + inputPaths[i]);
        GDALDatasetH srcDs = datasetHandle(input);
        inputs.push_back(std::move(input));

        const int srcX = crop.padLeft - crop.dx[i];
        const int srcY = crop.padTop - crop.dy[i];

        const int srcW = GDALGetRasterXSize(srcDs);
        const int srcH = GDALGetRasterYSize(srcDs);
        if (srcX < 0 || srcY < 0 ||
            srcX + crop.outW > srcW || srcY + crop.outH > srcH) {
            LOGD << "Invalid aligned crop window for band " << i
                 << ": srcwin=(" << srcX << ", " << srcY << ", "
                 << crop.outW << ", " << crop.outH << "), dataset=("
                 << srcW << ", " << srcH << ")";
            throw GDALException("Invalid aligned crop window for band " + std::to_string(i));
        }

        // An unnamed VRT: it reads the open input through its handle, and the VRT of the merge
        // reads it through its own (it opens no file by name)
        CPLStringList tArgs;
        tArgs.AddString("-of");
        tArgs.AddString("VRT");
        tArgs.AddString("-srcwin");
        tArgs.AddString(std::to_string(srcX).c_str());
        tArgs.AddString(std::to_string(srcY).c_str());
        tArgs.AddString(std::to_string(crop.outW).c_str());
        tArgs.AddString(std::to_string(crop.outH).c_str());

        const GdalOptionsPtr<GDALTranslateOptions> srcOpts(
            GDALTranslateOptionsNew(tArgs.List(), nullptr));

        auto window = adoptDataset(GDALTranslate("", srcDs, srcOpts.get(), nullptr));
        if (!window) {
            throw GDALException("Cannot create shifted dataset for band " + std::to_string(i));
        }
        windows.push_back(std::move(window));
    }

    return buildSeparateVrt(std::move(windows), std::move(inputs), "_aligned.vrt",
                            "Cannot build VRT for aligned merge");
}

}  // namespace ddb::merge
