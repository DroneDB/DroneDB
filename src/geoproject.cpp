/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "geoproject.h"
#include "mio.h"
#include <gdal_priv.h>
#include <gdal_utils.h>

namespace ddb {

void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize, bool stopOnError, const GeoProjectCallback &callback){
    bool isDirectory = fs::is_directory(output);
    bool outputToDir = images.size() > 1 || isDirectory;
    if (outputToDir){
        if (!isDirectory){
            // Bad input?
            if (fs::is_regular_file(output)){
                throw FSException(output + " is a file. (Did you switch the input and output parameters?)");
            }
            io::createDirectories(output);
        }
    }

    for (const std::string &img : images){
        fs::path p = img;
        if (!fs::exists(p)){
            throw FSException("Cannot project " + p.string() + " (does not exist)");
        }

        Entry e;
        parseEntry(p, ".", e, false);

        if (e.type != EntryType::GeoImage){
            if (stopOnError) throw FSException("Cannot geoproject " + p.string() + ", not a GeoImage");
            else std::cerr << "Cannot geoproject " << p.string() << ", not a GeoImage, skipping..." << std::endl;
            continue;
        }
        if (e.polygon_geom.size() < 4 || e.meta.find("width") == e.meta.end() || e.meta.find("height") == e.meta.end()){
            if (stopOnError) throw FSException("Cannot geoproject " + p.string() + ", the image does not have sufficient information");
            else std::cerr << "Cannot geoproject " << p.string() << ", the image does not have sufficient information: skipping" << std::endl;
            continue;
        }

        int width = e.meta["width"].get<int>();
        int height = e.meta["height"].get<int>();

        std::string outFile;
        if (outputToDir){
            outFile = (fs::path(output) / p.filename().replace_extension(".tif")).string();
        }else{
            outFile = output;
        }

        std::string tmpOutFile = fs::path(outFile).replace_extension(".tif.tmp").string();

        Point ul = e.polygon_geom.getPoint(0);
        Point ll = e.polygon_geom.getPoint(1);
        Point lr = e.polygon_geom.getPoint(2);
        Point ur = e.polygon_geom.getPoint(3);

        GDALDatasetH hSrcDataset = GDALOpen(p.string().c_str(), GA_ReadOnly);
        if (!hSrcDataset){
            if (stopOnError) throw FSException("Cannot project " + p.string() + ", cannot open raster");
            else std::cout << "Cannot project " << p.string() << ", cannot open raster: skipping" << std::endl;
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

        std::string vsiFilename = "/vsimem/";
        vsiFilename += p.filename().string() + "-" + std::to_string(rand()) + ".tif";
        GDALDatasetH hDstDataset = GDALTranslate(vsiFilename.c_str(),
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

        GDALDatasetH hWrpDataset = GDALWarp(tmpOutFile.c_str(),
                 nullptr,
                 1,
                 &hDstDataset,
                 waOptions,
                 nullptr);
        GDALWarpAppOptionsFree(waOptions);

        GDALClose(hSrcDataset);
        GDALClose(hDstDataset);
        GDALClose(hWrpDataset);

        fs::rename(tmpOutFile, outFile);

        if (callback){
            if (!callback(outFile)) return;
        }
    }
}

}
