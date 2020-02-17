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
#include "ddb.h"


namespace ddb{

void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool recursive, int maxRecursionDepth){
    std::vector<fs::path> filePaths;

    if (recursive){
        filePaths = getPathList(input, false, maxRecursionDepth);
    }else{
        filePaths = std::vector<fs::path>(input.begin(), input.end());
    }

    ParseEntryOpts peOpts;
    peOpts.withHash = false;
    peOpts.stopOnError = true;

    for (auto &fp : filePaths){
        LOGD << "Parsing entry " << fp.string();

        Entry e;
        if (entry::parseEntry(fp, "/", e, peOpts)){
            e.path = (fs::path("/") / fs::path(e.path)).string(); // TODO: does this work on Windows?
            if (e.type == Type::Image || e.type == Type::GeoImage || e.type == Type::GeoRaster){
                std::cout << generateThumb(e.path, e.mtime, thumbSize, output).string() << std::endl;
            }else{
                LOGD << "Skipping " << e.path;
            }
        }else{
            throw FSException("Failed to parse " + fp.string());
        }
    }
}

// Thumbnails are JPG files idenfitied by:
// sha256(imagePath + "*" + modifiedTime + "*" + thumbSize).jpg
//
// imagePath can be either absolute or relative and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path &imagePath, time_t modifiedTime, int thumbSize, const fs::path &outdir){
    if (!fs::is_directory(outdir)) throw FSException(outdir.string() + " is not a valid directory");
    if (!fs::exists(imagePath)) throw FSException(imagePath.string() + " does not exist");

    std::ostringstream os;
    os << imagePath.string() << "*" << modifiedTime << "*" << thumbSize;
    fs::path outPath = outdir / fs::path(Hash::ingestStr(os.str()) + ".jpg");

    // Check existance of thumbnail, return if exists
    if (fs::exists(outPath)){
        return outPath;
    }

    // Compute image with GDAL otherwise
    GDALDatasetH hSrcDataset = GDALOpen(imagePath.string().c_str(), GA_ReadOnly);
    if (!hSrcDataset){
        throw GDALException("Cannot open " + imagePath.string() + " for reading");
    }

    char** targs = nullptr;
    targs = CSLAddString(targs, "-outsize");
    targs = CSLAddString(targs, std::to_string(thumbSize).c_str());
    targs = CSLAddString(targs, "0");

    targs = CSLAddString(targs, "-co");
    targs = CSLAddString(targs, "WRITE_EXIF_METADATA=NO");

    targs = CSLAddString(targs, "--config");
    targs = CSLAddString(targs, "GDAL_PAM_ENABLED");
    targs = CSLAddString(targs, "NO");

    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
    CSLDestroy(targs);
    GDALTranslate(outPath.string().c_str(),
                                    hSrcDataset,
                                    psOptions,
                                    nullptr);
    GDALTranslateOptionsFree(psOptions);

    return outPath;
}

}
