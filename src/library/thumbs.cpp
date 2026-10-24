/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <sstream>

#include "cancellation.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "hash.h"
#include "mio.h"
#include "thumb_internal.h"
#include "timeutil.h"
#include "userprofile.h"

namespace ddb {

bool supportsThumbnails(EntryType type) {
    return entryHandler(type).thumbnailSource() == ThumbnailSource::File;
}

void generateThumbs(const std::vector<std::string>& input,
                    const fs::path& output,
                    int thumbSize,
                    bool useCrc,
                    const std::function<void(const fs::path&)>& onGenerated) {
    if (input.size() > 1)
        io::assureFolderExists(output);
    const bool outputIsFile = input.size() == 1 && io::Path(output).checkExtension(
                                                       {"jpg", "jpeg", "png", "webp", "json"});

    const std::vector<fs::path> filePaths = io::fromUtf8(input);

    for (auto& fp : filePaths) {
        LOGD << "Parsing entry " << io::toUtf8(fp);

        const EntryType type = fingerprint(fp);
        io::Path p(fp);

        // Files rendered by the thumbnail generator of an entry handler: build artifacts
        // (the .copc.laz of buildCopc(), the .rad of buildGsplat()) and .spz splats
        if (supportsThumbnails(type) || findThumbnailGenerator(fp) != nullptr) {
            fs::path outImagePath;
            if (useCrc) {
                outImagePath = output / getThumbFilename(fp, p.getModifiedTime(), thumbSize);
            } else if (outputIsFile) {
                outImagePath = output;
            } else {
                outImagePath = output / fs::path(fp).replace_extension(".webp").filename();
            }
            const fs::path generated = generateThumb(fp, thumbSize, outImagePath, true, nullptr, nullptr);
            if (onGenerated)
                onGenerated(generated);
        } else {
            LOGD << "Skipping " << fp;
        }
    }
}

fs::path getThumbFilename(const fs::path& imagePath, time_t modifiedTime, int thumbSize) {
    // Thumbnails are WEBP files idenfitied by:
    // CRC64(imagePath + "*" + modifiedTime + "*" + thumbSize).webp
    std::ostringstream os;
    os << io::toUtf8(imagePath) << "*" << modifiedTime << "*" << thumbSize;
    return io::fromUtf8(Hash::strCRC64(os.str()) + ".webp");
}

namespace thumb {

// WebP quality of every thumbnail: at preview sizes q80 is visually indistinguishable
// from q95 at about half the bytes and encode time
constexpr int WebpQuality = 80;

void write(const RasterImage& image,
           const fs::path& outImagePath,
           uint8_t** outBuffer,
           int* outBufferSize) {
    EncodeOptions options;
    options.format = ImageFormat::Webp;
    options.quality = WebpQuality;

    if (outImagePath.empty() && outBuffer != nullptr)
        TileEncoder::encode(image, options, outBuffer, outBufferSize);
    else
        TileEncoder::write(image, options, io::toUtf8(outImagePath));
}

}  // namespace thumb

// imagePath can be either absolute or relative or a network URL and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path& inputPath,
                       int thumbSize,
                       const fs::path& outImagePath,
                       bool forceRecreate,
                       uint8_t** outBuffer,
                       int* outBufferSize) {
    if (thumbSize <= 0)
        throw InvalidArgsException("thumbSize must be greater than 0");

    if (!io::isNetworkPath(io::toUtf8(inputPath)) && !exists(inputPath))
        throw FSException(io::toUtf8(inputPath) + " does not exist");

    // Writing the thumbnail replaces an existing file, so it must not be the input
    if (!io::isNetworkPath(io::toUtf8(inputPath)))
        io::assureDistinctOutput(inputPath, outImagePath);

    // Check existance of thumbnail, return if exists
    if (!io::isNetworkPath(io::toUtf8(inputPath)) && exists(outImagePath) && !forceRecreate)
        return outImagePath;

    LOGD << "ImagePath = " << inputPath;
    LOGD << "OutImagePath = " << outImagePath;
    LOGD << "Size = " << thumbSize;

    // COPC, RAD and SPZ files are rendered by their entry handler's generator, every other
    // file as an image (whose GDAL steps stop when the call is canceled, DQ-15)
    cancel::throwIfCanceled();
    if (const ThumbnailGenerator* generator = findThumbnailGenerator(inputPath))
        generator->generate(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);
    else
        generateImageThumb(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);

    return outImagePath;
}

void cleanupThumbsUserCache() {
    LOGD << "Cleaning up thumbs user cache";

    const time_t threshold = currentUnixTimestamp() - 60 * 60 * 24 * 5;  // 5 days
    const fs::path thumbsDir = UserProfile::get()->getThumbsDir();
    std::vector<fs::path> cleanupDirs;

    // Iterate size directories
    for (auto sd = fs::recursive_directory_iterator(thumbsDir);
         sd != fs::recursive_directory_iterator();
         ++sd) {
        fs::path sizeDir = sd->path();
        if (is_directory(sizeDir)) {
            for (auto t = fs::recursive_directory_iterator(sizeDir);
                 t != fs::recursive_directory_iterator();
                 ++t) {
                fs::path thumb = t->path();
                if (io::Path(thumb).getModifiedTime() < threshold) {
                    if (fs::remove(thumb))
                        LOGD << "Cleaned " << io::toUtf8(thumb);
                    else
                        LOGD << "Cannot clean " << io::toUtf8(thumb);
                }
            }

            if (is_empty(sizeDir)) {
                // Remove directory too
                cleanupDirs.push_back(sizeDir);
            }
        }
    }

    for (auto& d : cleanupDirs) {
        if (fs::remove(d))
            LOGD << "Cleaned " << io::toUtf8(d);
        else
            LOGD << "Cannot clean " << io::toUtf8(d);
    }
}

}  // namespace ddb
