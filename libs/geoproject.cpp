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

void geoProject(const std::vector<std::string> &images, const std::string &output){
    bool isDirectory = fs::is_directory(output);
    bool outputToDir = images.size() > 1 || isDirectory;
    if (outputToDir){
        if (!isDirectory) throw FSException(output + " is not a valid directory.");
    }

    auto imagePaths = getPathList("/", images, false);
    // TODO: remove getPathList ?!

//    auto q = db->query(R"<<<(
//                       WITH bounds AS (
//                            SELECT ExteriorRing(polygon_geom) as geom,
//                                   json_extract(meta, '$.imageWidth') as width,
//                                   json_extract(meta, '$.imageHeight') as height
//                            FROM entries
//                            WHERE type=? AND path=? AND
//                                  polygon_geom IS NOT NULL AND
//                                  width IS NOT NULL AND
//                                  height IS NOT NULL
//                       )
//                       SELECT  X(PointN(geom, 1)), Y(PointN(geom, 1)),
//                               X(PointN(geom, 2)), Y(PointN(geom, 2)),
//                               X(PointN(geom, 3)), Y(PointN(geom, 3)),
//                               X(PointN(geom, 4)), Y(PointN(geom, 4)),
//                               width, height
//                       FROM bounds
//                       )<<<");

    for (auto &ip : imagePaths){
        fs::path relPath = fs::relative(ip, directory);
        if (!fs::exists(ip)){
            std::cout << "Cannot project " << ip.string() << ", the image does not exist: skipping" << std::endl;
            continue;
        }

        q->bind(1, Type::GeoImage);
        q->bind(2, relPath.generic_string());

        if (q->fetch()) {
            geo::Point2D ul(q->getDouble(0), q->getDouble(1));
            geo::Point2D ll(q->getDouble(2), q->getDouble(3));
            geo::Point2D lr(q->getDouble(4), q->getDouble(5));
            geo::Point2D ur(q->getDouble(6), q->getDouble(7));

            int width = q->getInt(8);
            int height = q->getInt(9);

            std::string outFile;
            if (outputToDir){
                outFile = fs::path(output) / relPath.filename().replace_extension(".tif");
            }else{
                outFile = output;
            }

            GDALDatasetH hSrcDataset = GDALOpen(ip.string().c_str(), GA_ReadOnly);
            if (!hSrcDataset){
                std::cout << "Cannot project " << ip.string() << ", cannot open raster: skipping" << std::endl;
                continue;
            }

            char** targs = nullptr;
            targs = CSLAddString(targs, "-a_srs");
            targs = CSLAddString(targs, "EPSG:4326");

            targs = CSLAddString(targs, "-gcp");
            targs = CSLAddString(targs, "0");
            targs = CSLAddString(targs, "0");
            targs = CSLAddString(targs, utils::to_str(ul.x, 13).c_str());
            targs = CSLAddString(targs, utils::to_str(ul.y, 13).c_str());

            targs = CSLAddString(targs, "-gcp");
            targs = CSLAddString(targs, "0");
            targs = CSLAddString(targs, std::to_string(height).c_str());
            targs = CSLAddString(targs, utils::to_str(ll.x, 13).c_str());
            targs = CSLAddString(targs, utils::to_str(ll.y, 13).c_str());

            targs = CSLAddString(targs, "-gcp");
            targs = CSLAddString(targs, std::to_string(width).c_str());
            targs = CSLAddString(targs, std::to_string(height).c_str());
            targs = CSLAddString(targs, utils::to_str(lr.x, 13).c_str());
            targs = CSLAddString(targs, utils::to_str(lr.y, 13).c_str());

            targs = CSLAddString(targs, "-gcp");
            targs = CSLAddString(targs, std::to_string(width).c_str());
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
        }else{
            std::cout << "Cannot project " << relPath.string() << ", the image does not have sufficient information: skipping" << std::endl;
        }

        q->reset();
    }
}

}
