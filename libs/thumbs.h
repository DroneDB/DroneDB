#ifndef THUMBS_H
#define THUMBS_H

#include <filesystem>

namespace fs = std::filesystem;

namespace ddb{

void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool recursive, int maxRecursionDepth);
fs::path generateThumb(const fs::path &imagePath, time_t modifiedTime, int thumbSize, const fs::path &outdir);

}

#endif // THUMBS_H
