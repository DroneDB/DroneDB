/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "merge_vrt.h"

#include "exceptions.h"
#include "artefact_cache.h"

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
                             std::vector<VsiMemFile> sourceFiles, const std::string &suffix,
                             const std::string &error) {
    SeparateVrt v;
    v.sourceFiles = std::move(sourceFiles);
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

}  // namespace ddb::merge
