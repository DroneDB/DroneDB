/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <pdal/StageFactory.hpp>
#include <gdal_priv.h>
#include <ogr_srs_api.h>
#include <untwine/untwine/Common.hpp>
#include <untwine/untwine/ProgressWriter.hpp>
#include <untwine/epf/Epf.hpp>
#include <untwine/bu/BuPyramid.hpp>

#include "pointcloud.h"
#include "entry.h"
#include "exceptions.h"
#include "mio.h"
#include "geo.h"
#include "logger.h"
#include "net/functions.h"

namespace untwine{

void fatal(const std::string& err){
    throw ddb::UntwineException("Untwine fatal error: " + err);
}

}

namespace ddb{

bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info, int polyBoundsSrs){
    
    LOGD << "In getPointCloudInfo(" << filename << ")";

    try {

        pdal::StageFactory factory;
        std::string driver = factory.inferReaderDriver(filename);
        if (driver.empty()){
            LOGD << "Can't infer point cloud reader from " << filename;
            return false;
        }

        pdal::Stage *s = factory.createStage(driver);
        pdal::Options opts;
        opts.add("filename", filename);
        s->setOptions(opts);
        
        pdal::QuickInfo qi = s->preview();
        if (!qi.valid()){
            LOGD << "Cannot get quick info for point cloud " << filename;
            return false;
        }

        info.pointCount = qi.m_pointCount;

        if (qi.m_srs.valid()){
            info.wktProjection = qi.m_srs.getWKT();
        }else{
            info.wktProjection = "";
        }

        info.dimensions.clear();
        for (auto &dim : qi.m_dimNames){
            info.dimensions.push_back(dim);
        }

        info.bounds.clear();
        if (qi.m_bounds.valid()){
            info.bounds.push_back(qi.m_bounds.minx);
            info.bounds.push_back(qi.m_bounds.miny);
            info.bounds.push_back(qi.m_bounds.minz);
            info.bounds.push_back(qi.m_bounds.maxx);
            info.bounds.push_back(qi.m_bounds.maxy);
            info.bounds.push_back(qi.m_bounds.maxz);

            pdal::BOX3D bbox = qi.m_bounds;

            // We need to convert the bbox to EPSG:<polyboundsSrs>
            if (qi.m_srs.valid()){
                OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
                OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);

                std::string proj = qi.m_srs.getProj4();
                if (OSRImportFromProj4(hSrs, proj.c_str()) != OGRERR_NONE){
                    LOGD << "Cannot import spatial reference system " + proj + ". Is PROJ available?";

                    throw GDALException("Cannot import spatial reference system " + proj + ". Is PROJ available?");
                }
                OSRImportFromEPSG(hTgt, polyBoundsSrs);
                OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hTgt);

                double geoMinX = bbox.minx;
                double geoMinY = bbox.miny;
                double geoMinZ = bbox.minz;
                double geoMaxX = bbox.maxx;
                double geoMaxY = bbox.maxy;
                double geoMaxZ = bbox.maxz;

                bool minSuccess = OCTTransform(hTransform, 1, &geoMinX, &geoMinY, &geoMinZ);
                bool maxSuccess = OCTTransform(hTransform, 1, &geoMaxX, &geoMaxY, &geoMaxZ);

                if (!minSuccess || !maxSuccess){
                    LOGD << "Cannot transform coordinates";
                    throw GDALException("Cannot transform coordinates " + bbox.toWKT() + " to " + proj);
                }

                info.polyBounds.clear();
                info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);
                info.polyBounds.addPoint(geoMinY, geoMaxX, geoMinZ);
                info.polyBounds.addPoint(geoMaxY, geoMaxX, geoMinZ);
                info.polyBounds.addPoint(geoMaxY, geoMinX, geoMinZ);
                info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);

                double centroidX = (bbox.minx + bbox.maxx) / 2.0;
                double centroidY = (bbox.miny + bbox.maxy) / 2.0;
                double centroidZ = bbox.minz;

                if (OCTTransform(hTransform, 1, &centroidX, &centroidY, &centroidZ)){
                    info.centroid.clear();
                    info.centroid.addPoint(centroidY, centroidX, centroidZ);
                }else{
                    throw GDALException("Cannot transform coordinates " + std::to_string(centroidX) + ", " + std::to_string(centroidY) + " to " + proj);
                }

                OCTDestroyCoordinateTransformation(hTransform);
                OSRDestroySpatialReference(hTgt);
                OSRDestroySpatialReference(hSrs);
            }
        }

    } catch(GDALException& e) {
        throw;
    } catch(std::runtime_error& e) {
        LOGD << "Exception: " << e.what();
        return false;
    }

    return true;
}

bool getEptInfo(const std::string &eptJson, PointCloudInfo &info, int polyBoundsSrs, int *span){
    json j;
    try{
        j = json::parse(net::readFile(eptJson));
    }catch(json::exception &e){
        LOGD << e.what();
        return false;
    }catch(const FSException &e){
        LOGD << e.what();
        return false;
    }catch(const NetException &e){
        LOGD << e.what();
        return false;
    }

    if (j.contains("boundsConforming") &&
        j.contains("points") &&
        j.contains("srs") &&
        j.contains("schema") &&
        j.contains("span")){

        info.pointCount = j["points"];

        if (j["srs"].contains("wkt")){
            info.wktProjection = j["srs"]["wkt"];
        }else{
            info.wktProjection = "";
        }

        info.dimensions.clear();
        for (auto &dim : j["schema"]){
            if (dim.contains("name")){
                info.dimensions.push_back(dim["name"]);
            }
        }

        if (span != nullptr){
            *span = j["span"];
        }

        double minx = j["boundsConforming"][0];
        double miny = j["boundsConforming"][1];
        double minz = j["boundsConforming"][2];
        double maxx = j["boundsConforming"][3];
        double maxy = j["boundsConforming"][4];
        double maxz = j["boundsConforming"][5];

        info.bounds.clear();
        info.bounds.push_back(minx);
        info.bounds.push_back(miny);
        info.bounds.push_back(minz);
        info.bounds.push_back(maxx);
        info.bounds.push_back(maxy);
        info.bounds.push_back(maxz);

        if (!info.wktProjection.empty()){
            OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
            OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);

            char *wkt = strdup(info.wktProjection.c_str());
            if (OSRImportFromWkt(hSrs, &wkt) != OGRERR_NONE){
                throw GDALException("Cannot import spatial reference system " + info.wktProjection + ". Is PROJ available?");
            }
            OSRImportFromEPSG(hTgt, polyBoundsSrs);
            OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hTgt);

            double geoMinX = minx;
            double geoMinY = miny;
            double geoMinZ = minz;
            double geoMaxX = maxx;
            double geoMaxY = maxy;
            double geoMaxZ = maxz;

            bool minSuccess = OCTTransform(hTransform, 1, &geoMinX, &geoMinY, &geoMinZ);
            bool maxSuccess = OCTTransform(hTransform, 1, &geoMaxX, &geoMaxY, &geoMaxZ);

            if (!minSuccess || !maxSuccess){
                throw GDALException("Cannot transform coordinates " + info.wktProjection + " to EPSG:" + std::to_string(polyBoundsSrs));
            }

            info.polyBounds.clear();
            info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);
            info.polyBounds.addPoint(geoMinY, geoMaxX, geoMinZ);
            info.polyBounds.addPoint(geoMaxY, geoMaxX, geoMinZ);
            info.polyBounds.addPoint(geoMaxY, geoMinX, geoMinZ);
            info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);

            double centroidX = (minx + maxx) / 2.0;
            double centroidY = (miny + maxy) / 2.0;
            double centroidZ = minz;

            if (OCTTransform(hTransform, 1, &centroidX, &centroidY, &centroidZ)){
                info.centroid.clear();
                info.centroid.addPoint(centroidY, centroidX, centroidZ);
            }else{
                throw GDALException("Cannot transform coordinates " + std::to_string(centroidX) + ", " + std::to_string(centroidY) + " to EPSG:" + std::to_string(polyBoundsSrs));
            }

            OCTDestroyCoordinateTransformation(hTransform);
            OSRDestroySpatialReference(hTgt);
            OSRDestroySpatialReference(hSrs);
        }

        return true;
    }else{
        LOGD << "Invalid EPT: " << eptJson;
        return false;
    }
}

void buildEpt(const std::vector<std::string> &filenames, const std::string &outdir){
    fs::path dest = outdir;
    fs::path tmpDir = dest / "tmp";

    untwine::Options options;
    for (const std::string &f : filenames){
        if (!fs::exists(f)) throw FSException(f + " does not exist");

        const EntryType type = fingerprint(f);
        if (type != EntryType::PointCloud) throw InvalidArgsException(f + " is not a supported point cloud file");

        options.inputFiles.push_back(f);
    }
    options.tempDir = tmpDir.string();
    options.outputDir = dest.string();
    options.fileLimit = 10000000;
    options.progressFd = -1;
    options.stats = false;
    options.level = -1;

    io::assureFolderExists(dest);
    io::assureFolderExists(tmpDir);
    io::assureIsRemoved(dest / "ept.json");
    io::assureIsRemoved(dest / "ept-data");
    io::assureIsRemoved(dest / "ept-hierarchy");
    io::assureFolderExists(dest / "ept-data");
    io::assureFolderExists(dest / "ept-hierarchy");


    untwine::ProgressWriter progress(options.progressFd);

    try{
        untwine::BaseInfo common;

        untwine::epf::Epf preflight(common);
        preflight.run(options, progress);

        untwine::bu::BuPyramid builder(common);
        builder.run(options, progress);

        io::assureIsRemoved(tmpDir);
        io::assureIsRemoved(dest / "temp");
    }catch (const std::exception &e){
        throw UntwineException(e.what());
    }
}


json PointCloudInfo::toJSON(){
    json j;
    j["pointCount"] = pointCount;
    j["projection"] = wktProjection;
    j["dimensions"] = dimensions;

    return j;
}

}
