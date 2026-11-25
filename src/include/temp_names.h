/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TEMP_NAMES_H
#define TEMP_NAMES_H

#include <array>
#include <string_view>

namespace ddb::tempnames
{
    // The names libddb starts the folders it makes in the temporary folder of the system
    // (fs::temp_directory_path, which the Registry points at its own folder) with, followed by random
    // characters. A folder is removed when its work ends; one a crash left keeps its name, which is how
    // the cleanup of the cache (cache_cleanup.h) knows it is libddb's.
    inline constexpr const char *kObjStage = "ddb_obj_stage_";
    inline constexpr const char *kAssimpConvert = "ddb_assimp_convert_";
    inline constexpr const char *kPlyConvert = "ddb_ply_convert_";
    inline constexpr const char *kThumbnail = "ddb-thumb-";
    inline constexpr const char *kMergePreview = "ddb-merge-preview-";
    inline constexpr const char *kRender = "ddb-render-";
    // The name earlier versions gave the folder of a model converted for a 3D build
    inline constexpr const char *kLegacyGltfConvert = "ddb_gltf_convert_";
    // The temporary file of the GDAL COG driver that built cog/cog.tif in the process of earlier versions
    // (CPLGenerateTempFilename: the name of the output, then _<pid>_<counter>), about the size of the COG;
    // a build that was stopped left it behind
    inline constexpr const char *kLegacyCogBuild = "cog.tif_";

    inline constexpr std::array<std::string_view, 8> kAll = {
        kObjStage, kAssimpConvert, kPlyConvert, kThumbnail, kMergePreview, kRender, kLegacyGltfConvert, kLegacyCogBuild};
} // namespace ddb::tempnames

#endif // TEMP_NAMES_H
