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
#include "thumbs.h"
#include "../classes/exceptions.h"

using namespace ddb;

//bool getThumbs(const std::vector<std::string> &input){

//}


// Thumbnails are JPG files idenfitied by:
// sha256(imagePath + "*" + modifiedTime + "*" + thumbSize).jpg
//
// imagePath can be either absolute or relative and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path &imagePath, time_t modifiedTime, int thumbSize, const fs::path &outdir){
    if (!fs::is_directory(outdir)) throw FSException(outdir.string() + " is not a valid directory");
    if (!fs::exists(imagePath)) throw FSException(imagePath.string() + " does not exist");

    // TODO compute hash
    // check existance of thumbnail, return if exists
    // compute image with GDAL otherwise
}

