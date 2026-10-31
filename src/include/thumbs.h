/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THUMBS_H
#define THUMBS_H

#include "entry.h"
#include "fs.h"

#include <cstddef>
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
    // What a Gaussian Splat thumbnail drew, for the tests and tools that look into the renderer
    struct SplatThumbStats
    {
        size_t splats = 0;           // read, with a finite position and some opacity
        size_t framed = 0;           // the subject: the splats the camera is aimed at and fitted to
        bool enclosed = false;       // a room: seen from higher up, with its top and its near side cut away
        size_t drawn = 0;            // projected into the picture
        size_t cutAway = 0;          // between the camera and the subject, far behind it, in the sky above it or
                                     // under its ground; the top and the near side of a room
        size_t culledOversized = 0;  // a footprint too wide for the picture
        size_t pixelBufferBytes = 0; // the buffers that grow with the picture, beyond the picture itself
    };

    // Thumbnails of a Gaussian Splat, from a .spz file or from the level-of-detail .rad artifact.
    // thumbSize is at most 4096 (InvalidArgsException); `stats`, when given, says what was drawn.
    void generateSplatThumb(const fs::path &spzPath, int thumbSize,
                            const fs::path &outImagePath,
                            uint8_t **outBuffer, int *outBufferSize,
                            SplatThumbStats *stats = nullptr);
    void generateSplatThumbFromRad(const fs::path &radPath, int thumbSize,
                                   const fs::path &outImagePath,
                                   uint8_t **outBuffer, int *outBufferSize,
                                   SplatThumbStats *stats = nullptr);
    void cleanupThumbsUserCache();

}

#endif // THUMBS_H
