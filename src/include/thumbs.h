/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THUMBS_H
#define THUMBS_H

#include "entry.h"
#include "fs.h"
#include "ddb_export.h"

#include <string>
#include <vector>

namespace ddb
{

    struct ThumbVisParams {
        std::string preset;      // Preset ID (e.g. "true-color")
        std::string bands;       // Comma-separated band indices (e.g. "4,3,2")
        std::string formula;     // Formula ID (e.g. "NDVI")
        std::string bandFilter;  // Band order (e.g. "RGBNRe")
        std::string colormap;    // Colormap ID (e.g. "rdylgn")
        std::string rescale;     // Rescale range "min,max"
    };

    DDB_DLL fs::path getThumbFromUserCache(const fs::path &imagePath, int thumbSize, bool forceRecreate);
    DDB_DLL void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool useCrc);
    DDB_DLL bool supportsThumbnails(EntryType type);
    DDB_DLL fs::path getThumbFilename(const fs::path &imagePath, time_t modifiedTime, int thumbSize);
    DDB_DLL fs::path generateThumb(const fs::path &imagePath, int thumbSize, const fs::path &outImagePath, bool forceRecreate, uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr);
    DDB_DLL void generateImageThumbEx(const fs::path &imagePath, int thumbSize,
                                       const fs::path &outImagePath,
                                       uint8_t **outBuffer, int *outBufferSize,
                                       const ThumbVisParams &visParams);
    DDB_DLL void cleanupThumbsUserCache();

}

#endif // THUMBS_H
