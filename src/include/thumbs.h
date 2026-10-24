/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THUMBS_H
#define THUMBS_H

#include "entry.h"
#include "fs.h"

#include <cstdint>
#include <functional>
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

    // Writes a thumbnail per file the generators support; onGenerated gets the path of each one
    // as soon as it is written
    void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool useCrc,
                        const std::function<void(const fs::path &)> &onGenerated = {});
    bool supportsThumbnails(EntryType type);
    fs::path getThumbFilename(const fs::path &imagePath, time_t modifiedTime, int thumbSize);
    fs::path generateThumb(const fs::path &imagePath, int thumbSize, const fs::path &outImagePath, bool forceRecreate, uint8_t **outBuffer = nullptr, int *outBufferSize = nullptr);
    void generateImageThumbEx(const fs::path &imagePath, int thumbSize,
                                       const fs::path &outImagePath,
                                       uint8_t **outBuffer, int *outBufferSize,
                                       const ThumbVisParams &visParams);
    // Thumbnail of a COPC point cloud. It reads the octree down to the deepest level
    // whose points fit pointBudget (0 = about 2 points per output pixel).
    void generatePointCloudThumb(const fs::path &copcPath, int thumbSize,
                                 const fs::path &outImagePath,
                                 uint8_t **outBuffer, int *outBufferSize,
                                 uint64_t pointBudget = 0);
    // Thumbnails of a Gaussian Splat, from a .spz file or from the level-of-detail .rad artifact
    void generateSplatThumb(const fs::path &spzPath, int thumbSize,
                            const fs::path &outImagePath,
                            uint8_t **outBuffer, int *outBufferSize);
    void generateSplatThumbFromRad(const fs::path &radPath, int thumbSize,
                                   const fs::path &outImagePath,
                                   uint8_t **outBuffer, int *outBufferSize);
    void cleanupThumbsUserCache();

}

#endif // THUMBS_H
