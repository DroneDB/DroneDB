#ifndef THUMBS_H
#define THUMBS_H

#include <filesystem>

namespace fs = std::filesystem;

namespace ddb{

void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool useCrc);
fs::path getThumbPath(const fs::path &imagePath, time_t modifiedTime, int thumbSize, const fs::path &outdir);
fs::path generateThumb(const fs::path &imagePath, int thumbSize, const fs::path &outImagePath, bool forceRecreate);
}

#endif // THUMBS_H
