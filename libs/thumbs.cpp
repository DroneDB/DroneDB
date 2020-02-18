/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include <sstream>
#include <gdal_priv.h>
#include <gdal_utils.h>

#include "thumbs.h"
#include "../classes/exceptions.h"
#include "../classes/hash.h"
#include "../utils.h"
#include "../classes/userprofile.h"
#include "ddb.h"


namespace ddb{

fs::path getThumbFromUserCache(const fs::path &imagePath, time_t modifiedTime, int thumbSize, bool forceRecreate){
    fs::path outdir = UserProfile::get()->getThumbsDir(thumbSize);
    fs::path thumbPath = outdir / getThumbFilename(imagePath, modifiedTime, thumbSize);

    return generateThumb(imagePath, thumbSize, thumbPath, forceRecreate);
}

bool supportsThumbnails(entry::Type type){
    return type == entry::Type::Image || type == entry::Type::GeoImage || type == entry::Type::GeoRaster;
}

void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool useCrc){
    if (!fs::is_directory(output)) throw FSException(output.string() + " is not a valid directory");

    std::vector<fs::path> filePaths = std::vector<fs::path>(input.begin(), input.end());

    ParseEntryOpts peOpts;
    peOpts.withHash = false;
    peOpts.stopOnError = true;

    for (auto &fp : filePaths){
        LOGD << "Parsing entry " << fp.string();

        Entry e;
        if (entry::parseEntry(fp, "/", e, peOpts)){
            e.path = (fs::path("/") / fs::path(e.path)).string(); // TODO: does this work on Windows?
            if (supportsThumbnails(e.type)){
                fs::path outImagePath;
                if (useCrc){
                    outImagePath = output / getThumbFilename(e.path, e.mtime, thumbSize);
                }else{
                    outImagePath = output / fs::path(e.path).replace_extension(".jpg").filename();
                }
                std::cout << generateThumb(e.path, thumbSize, outImagePath, true).string() << std::endl;
            }else{
                LOGD << "Skipping " << e.path;
            }
        }else{
            throw FSException("Failed to parse " + fp.string());
        }
    }
}


fs::path getThumbFilename(const fs::path &imagePath, time_t modifiedTime, int thumbSize){
    // Thumbnails are JPG files idenfitied by:
    // sha256(imagePath + "*" + modifiedTime + "*" + thumbSize).jpg
    std::ostringstream os;
    os << imagePath.string() << "*" << modifiedTime << "*" << thumbSize;
    return fs::path(Hash::strCRC64(os.str()) + ".jpg");
}


// imagePath can be either absolute or relative and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path &imagePath, int thumbSize, const fs::path &outImagePath, bool forceRecreate){
    if (!fs::exists(imagePath)) throw FSException(imagePath.string() + " does not exist");

    // Check existance of thumbnail, return if exists
    if (fs::exists(outImagePath) && !forceRecreate){
        return outImagePath;
    }

    // Compute image with GDAL otherwise
    GDALDatasetH hSrcDataset = GDALOpen(imagePath.string().c_str(), GA_ReadOnly);
    if (!hSrcDataset){
        throw GDALException("Cannot open " + imagePath.string() + " for reading");

    }

    int width = GDALGetRasterXSize(hSrcDataset);
    int height = GDALGetRasterYSize(hSrcDataset);
    int targetWidth = 0;
    int targetHeight = 0;
    if (width > height){
        targetWidth = thumbSize;
        targetHeight = static_cast<int>((static_cast<float>(thumbSize) / static_cast<float>(width)) * static_cast<float>(height));
    }else{
        targetHeight = thumbSize;
        targetWidth = static_cast<int>((static_cast<float>(thumbSize) / static_cast<float>(height)) * static_cast<float>(width));
    }

    char** targs = nullptr;
    targs = CSLAddString(targs, "-outsize");
    targs = CSLAddString(targs, std::to_string(targetWidth).c_str());
    targs = CSLAddString(targs, std::to_string(targetHeight).c_str());

    targs = CSLAddString(targs, "-ot");
    targs = CSLAddString(targs, "Byte");

    targs = CSLAddString(targs, "-scale");

    targs = CSLAddString(targs, "-co");
    targs = CSLAddString(targs, "WRITE_EXIF_METADATA=NO");

    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO"); // avoid aux files for PNG tiles

    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
    CSLDestroy(targs);
    GDALTranslate(outImagePath.string().c_str(),
                                    hSrcDataset,
                                    psOptions,
                                    nullptr);
    GDALTranslateOptionsFree(psOptions);

    return outImagePath;
}

}
