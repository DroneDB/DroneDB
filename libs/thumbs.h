/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THUMBS_H
#define THUMBS_H

#include "entry.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace ddb{

fs::path getThumbFromUserCache(const fs::path &imagePath, time_t modifiedTime, int thumbSize, bool forceRecreate);
void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool useCrc);
bool supportsThumbnails(entry::Type type);
fs::path getThumbFilename(const fs::path &imagePath, time_t modifiedTime, int thumbSize);
fs::path generateThumb(const fs::path &imagePath, int thumbSize, const fs::path &outImagePath, bool forceRecreate);
}

#endif // THUMBS_H
