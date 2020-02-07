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

#include "ddb.h"
#include "geoproject.h"
#include <gdal_priv.h>
#include <gdal_utils.h>

namespace ddb {

void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize){
    bool isDirectory = fs::is_directory(output);
    bool outputToDir = images.size() > 1 || isDirectory;
    if (outputToDir){
        if (!isDirectory){
            if (!fs::create_directories(output)){
                throw FSException(output + " is not a valid directory (cannot create it).");
            }
        }
    }

    for (const fs::path &p : images){
        if (!fs::exists(p)){
            throw FSException("Cannot project " + p.string() + " (does not exist)");
        }

        entry::Entry e;
        if (!parseEntry(p, ".", e, false)){
            throw FSException("Cannot parse file " + p.string());
        }

        if (e.type != entry::Type::GeoImage){
            std::cerr << "Cannot reproject " << p.string() << ", not a GeoImage, skipping..." << std::endl;
            continue;
        }
        if (e.polygon_geom.size() < 4 || e.meta.find("imageWidth") == e.meta.end() || e.meta.find("imageHeight") == e.meta.end()){
            std::cerr << "Cannot project " << p.string() << ", the image does not have sufficient information: skipping" << std::endl;
            continue;
        }

        int width = e.meta["imageWidth"].get<int>();
        int height = e.meta["imageHeight"].get<int>();

        std::string outFile;
        if (outputToDir){
            outFile = fs::path(output) / p.filename().replace_extension(".tif");
        }else{
            outFile = output;
        }

        Point ul = e.polygon_geom.getPoint(0);
        Point ll = e.polygon_geom.getPoint(1);
        Point lr = e.polygon_geom.getPoint(2);
        Point ur = e.polygon_geom.getPoint(3);

        GDALDatasetH hSrcDataset = GDALOpen(p.string().c_str(), GA_ReadOnly);
        if (!hSrcDataset){
            std::cout << "Cannot project " << p.string() << ", cannot open raster: skipping" << std::endl;
            continue;
        }

        char** targs = nullptr;
        targs = CSLAddString(targs, "-a_srs");
        targs = CSLAddString(targs, "EPSG:4326");

        int scaledWidth = width;
        int scaledHeight = height;


        if (outsize.length() > 0){
            double ratio = 1.0;

            targs = CSLAddString(targs, "-outsize");
            targs = CSLAddString(targs, outsize.c_str());
            if (outsize.back() == '%'){
                targs = CSLAddString(targs, outsize.c_str());
                ratio = std::stod(outsize) / 100.0;
            }else{
                ratio = std::stod(outsize) / width;
                targs = CSLAddString(targs, utils::to_str(ratio * height).c_str());
            }

            scaledWidth = static_cast<int>(static_cast<double>(width) * ratio);
            scaledHeight = static_cast<int>(static_cast<double>(height) * ratio);

            LOGD << "Scaled width: " << scaledWidth;
            LOGD << "Scaled height: " << scaledHeight;
        }

        targs = CSLAddString(targs, "-gcp");
        targs = CSLAddString(targs, "0");
        targs = CSLAddString(targs, "0");
        targs = CSLAddString(targs, utils::to_str(ul.x, 13).c_str());
        targs = CSLAddString(targs, utils::to_str(ul.y, 13).c_str());

        targs = CSLAddString(targs, "-gcp");
        targs = CSLAddString(targs, "0");
        targs = CSLAddString(targs, std::to_string(scaledHeight).c_str());
        targs = CSLAddString(targs, utils::to_str(ll.x, 13).c_str());
        targs = CSLAddString(targs, utils::to_str(ll.y, 13).c_str());

        targs = CSLAddString(targs, "-gcp");
        targs = CSLAddString(targs, std::to_string(scaledWidth).c_str());
        targs = CSLAddString(targs, std::to_string(scaledHeight).c_str());
        targs = CSLAddString(targs, utils::to_str(lr.x, 13).c_str());
        targs = CSLAddString(targs, utils::to_str(lr.y, 13).c_str());

        targs = CSLAddString(targs, "-gcp");
        targs = CSLAddString(targs, std::to_string(scaledWidth).c_str());
        targs = CSLAddString(targs, "0");
        targs = CSLAddString(targs, utils::to_str(ur.x, 13).c_str());
        targs = CSLAddString(targs, utils::to_str(ur.y, 13).c_str());

        GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
        CSLDestroy(targs);
        GDALDatasetH hDstDataset = GDALTranslate("/vsimem/translated.tif",
                                                hSrcDataset,
                                                psOptions,
                                                nullptr);
        GDALTranslateOptionsFree(psOptions);

        // Run gdalwarp to add trasparency, apply GCPs
        char** wargs = nullptr;
        wargs = CSLAddString(wargs, "-of");
        wargs = CSLAddString(wargs, "GTiff");
        wargs = CSLAddString(wargs, "-co");
        wargs = CSLAddString(wargs, "COMPRESS=JPEG");
        wargs = CSLAddString(wargs, "-dstalpha");
        GDALWarpAppOptions* waOptions = GDALWarpAppOptionsNew(wargs, nullptr);
        CSLDestroy(wargs);

        GDALDatasetH hWrpDataset = GDALWarp(outFile.c_str(),
                 nullptr,
                 1,
                 &hDstDataset,
                 waOptions,
                 nullptr);
        GDALWarpAppOptionsFree(waOptions);

        std::cout << "W\t" << outFile << std::endl;

        GDALClose(hSrcDataset);
        GDALClose(hDstDataset);
        GDALClose(hWrpDataset);
    }
}

}
