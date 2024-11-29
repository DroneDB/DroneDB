/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <exiv2/exiv2.hpp>
#include "dbops.h"
#include "entry.h"

#include <ddb.h>

#include "mio.h"
#include "pointcloud.h"
#include "ply.h"
#include "ogr_srs_api.h"

namespace ddb {

void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool withHash) {
    entry.type = EntryType::Undefined;

    try {
        if (!fs::exists(path)) throw FSException(path.string() + " does not exist");
    } catch (const fs::filesystem_error &e) {
        // Yes Windows will throw an exception in some cases :/
        throw FSException(e.what());
    }

    // Parse file
    io::Path p = io::Path(path);
    io::Path relPath = p.relativeTo(rootDirectory);

    entry.path = relPath.generic();
    entry.depth = relPath.depth();
    if (entry.mtime == 0) entry.mtime = p.getModifiedTime();

    if (fs::is_directory(path)) {
        entry.type = EntryType::Directory;
        entry.hash = "";
        entry.size = 0;

        // Check for DroneDB dir
        try{
            if (fs::exists(path / DDB_FOLDER / "dbase.sqlite")) {
                parseDroneDBEntry(path, entry);
            }
        }catch(const fs::filesystem_error &e){
            LOGD << "Cannot check " << path.string() << " .ddb presence: " << e.what();
        }
    } else {
        if (entry.hash == "" && withHash) entry.hash = Hash::fileSHA256(path.string());
        entry.size = p.getSize();
        entry.type = fingerprint(p.get());

        bool pano = entry.type == EntryType::Panorama || entry.type == EntryType::GeoPanorama;
        bool image = entry.type == EntryType::Image || entry.type == EntryType::GeoImage || pano;
        bool video = entry.type == EntryType::Video || entry.type == EntryType::GeoVideo;

        if (image || video) {
            try{
                auto exivImage = Exiv2::ImageFactory::open(path.string());
                if (!exivImage.get()) throw new IndexException("Cannot open " + path.string());

                exivImage->readMetadata();
                ExifParser e(exivImage.get());

                if (e.hasTags()) {
                    SensorSize sensorSize;
                    Focal focal;
                    CameraOrientation cameraOri;

                    ImageSize imageSize(0, 0);
                    if (image) imageSize = e.extractImageSize();
                    else if (video) imageSize = e.extractVideoSize();

                    entry.properties["width"] = imageSize.width;
                    entry.properties["height"] = imageSize.height;
                    entry.properties["captureTime"] = e.extractCaptureTime();

                    if (image){
                        entry.properties["orientation"] = e.extractImageOrientation();
                        entry.properties["make"] = e.extractMake();
                        entry.properties["model"] = e.extractModel();
                        entry.properties["sensor"] = e.extractSensor();

                        if (e.extractSensorSize(sensorSize)){
                            entry.properties["sensorWidth"] = sensorSize.width;
                            entry.properties["sensorHeight"] = sensorSize.height;
                        }

                        if (e.computeFocal(focal)){
                            entry.properties["focalLength"] = focal.length;
                            entry.properties["focalLength35"] = focal.length35;
                        }

                        e.extractCameraOrientation(cameraOri);
                        entry.properties["cameraYaw"] = cameraOri.yaw;
                        entry.properties["cameraPitch"] = cameraOri.pitch;
                        entry.properties["cameraRoll"] = cameraOri.roll;
                        LOGD << "Camera Orientation: " << cameraOri;
                    }

                    GeoLocation geo;
                    if (e.extractGeo(geo)) {
                        entry.point_geom.addPoint(geo.longitude, geo.latitude, geo.altitude);
                        LOGD << "POINT GEOM: "<< entry.point_geom.toWkt();

                        //e.printAllTags();

                        // Estimate image footprint
                        if (image && !pano){
                            double relAltitude = 0.0;

                            if (e.extractRelAltitude(relAltitude) && sensorSize.width > 0.0 && focal.length > 0.0) {
                                calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, entry.polygon_geom);
                            }
                        }
                    }

                    if (pano){
                        PanoramaInfo pInfo;
                        if (e.extractPanoramaInfo(pInfo)){
                            entry.properties["projectionType"] = pInfo.projectionType;
                            entry.properties["croppedWidth"] = pInfo.croppedWidth;
                            entry.properties["croppedHeight"] = pInfo.croppedHeight;
                            entry.properties["croppedX"] = pInfo.croppedX;
                            entry.properties["croppedY"] = pInfo.croppedY;
                            entry.properties["poseHeading"] = pInfo.poseHeading;
                            entry.properties["posePitch"] = pInfo.posePitch;
                            entry.properties["poseRoll"] = pInfo.poseRoll;
                        }
                    }
                } else {
                    LOGD << "No XMP/EXIF data found in " << path.string();
                }
            }catch(Exiv2::Error&){
                LOGD << "Cannot read EXIF data: " << path.string();
            }
        }else if (entry.type == EntryType::GeoRaster){
            GDALDatasetH  hDataset;
            hDataset = GDALOpen( path.string().c_str(), GA_ReadOnly );
            if (!hDataset)
                throw GDALException("Cannot open " + path.string() + " for reading");

            int width = GDALGetRasterXSize(hDataset);
            int height = GDALGetRasterYSize(hDataset);

            entry.properties["width"] = width;
            entry.properties["height"] = height;

            double geotransform[6];
            if (GDALGetGeoTransform(hDataset, geotransform) == CE_None){
                entry.properties["geotransform"] = json::array();
                for (int i = 0; i < 6; i++) entry.properties["geotransform"].push_back(geotransform[i]);

                if (GDALGetProjectionRef(hDataset) != NULL){
                    std::string wkt = GDALGetProjectionRef(hDataset);
                    if (!wkt.empty()){
                        // Set projection
                        entry.properties["projection"] = wkt;

                        // Get lat/lon extent of raster
                        char *wktp = const_cast<char *>(wkt.c_str());
                        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
                        OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);

                        if (OSRImportFromWkt(hSrs, &wktp) != OGRERR_NONE){
                            throw GDALException("Cannot read spatial reference system for " + path.string() + ". Is PROJ available?");
                        }
                        OSRSetAxisMappingStrategy(hSrs, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);

                        OSRImportFromEPSG(hWgs84, 4326);
                        OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hWgs84);

                        auto ul = getRasterCoordinate(hTransform, geotransform, 0.0, 0.0);
                        auto ur = getRasterCoordinate(hTransform, geotransform, width, 0);
                        auto lr = getRasterCoordinate(hTransform, geotransform, width, height);
                        auto ll = getRasterCoordinate(hTransform, geotransform, 0.0, height);

                        entry.polygon_geom.addPoint(ul.longitude, ul.latitude, 0.0);
                        entry.polygon_geom.addPoint(ur.longitude, ur.latitude, 0.0);
                        entry.polygon_geom.addPoint(lr.longitude, lr.latitude, 0.0);
                        entry.polygon_geom.addPoint(ll.longitude, ll.latitude, 0.0);
                        entry.polygon_geom.addPoint(ul.longitude, ul.latitude, 0.0);

                        auto center = getRasterCoordinate(hTransform, geotransform, width / 2.0, height / 2.0);
                        entry.point_geom.addPoint(center.longitude, center.latitude, 0.0);

                        OCTDestroyCoordinateTransformation(hTransform);
                        OSRDestroySpatialReference(hWgs84);
                        OSRDestroySpatialReference(hSrs);
                    }else{
                        LOGD << "Projection is empty";
                    }
                }
            }

            entry.properties["bands"] = json::array();
            for (int i = 0; i < GDALGetRasterCount(hDataset); i++){
                GDALRasterBandH hBand = GDALGetRasterBand(hDataset, i + 1);
                auto b = json::object();
                b["type"] = GDALGetDataTypeName(GDALGetRasterDataType(hBand));
                b["colorInterp"] = GDALGetColorInterpretationName(GDALGetRasterColorInterpretation(hBand));
                entry.properties["bands"].push_back(b);
            }

            GDALClose(hDataset);
        }else if (entry.type == EntryType::PointCloud){
            PointCloudInfo info;
            if (getPointCloudInfo(path.string(), info)){
                entry.properties = info.toJSON();
                entry.polygon_geom = info.polyBounds;
                entry.point_geom = info.centroid;
            }
        }
    }
}

Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, double *geotransform, double x, double y){
    double dfGeoX = geotransform[0] + geotransform[1] * x + geotransform[2] * y;
    double dfGeoY = geotransform[3] + geotransform[4] * x + geotransform[5] * y;

    if (OCTTransform(hTransform, 1, &dfGeoX, &dfGeoY, nullptr)){
        return Geographic2D(dfGeoY, dfGeoX);
    }else{
        throw GDALException("Cannot get raster coordinates of corner " + std::to_string(x) + "," + std::to_string(y));
    }
}

// Adapted from https://github.com/mountainunicycler/dronecamerafov/tree/master
void calculateFootprint(const SensorSize &sensorSize, const GeoLocation &geo, const Focal &focal, const CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom) {
    auto utmZone = getUTMZone(geo.latitude, geo.longitude);
    auto center = toUTM(geo.latitude, geo.longitude, utmZone);
    double groundHeight = geo.altitude != 0.0 ? geo.altitude - relAltitude : relAltitude;

    // Field of view

    // Wide
    double xView = 2.0 * atan(sensorSize.width / (2.0 * focal.length));

    // Tall
    double yView = 2.0 * atan(sensorSize.height / (2.0 * focal.length));

    // Cap pitch to 60 degrees
    double pitch = cameraOri.pitch;
    if (pitch > -30){
        LOGD << "Pitch cap exceeded (" << pitch << ") using nadir";
        pitch = -90; // set to nadir
    }

    // From drone to...
    double bottom = relAltitude * tan(utils::deg2rad(90.0 + pitch) - 0.5 * yView);
    double top = relAltitude * tan(utils::deg2rad(90.0 + pitch) + 0.5 * yView);
    double left = relAltitude * tan(utils::deg2rad(cameraOri.roll) - 0.5 * xView);
    double right = relAltitude * tan(utils::deg2rad(cameraOri.roll) + 0.5 * xView);
    // ... of picture.

//    LOGD << "xView: " << utils::rad2deg(xView);
//    LOGD << "yView: " << utils::rad2deg(yView);
//    LOGD << "bottom: " << bottom;
//    LOGD << "top: " << top;
//    LOGD << "left: " << left;
//    LOGD << "right: " << right;

    // Corners aligned north
    auto upperLeft = Projected2D(center.x + left, center.y + top);
    auto upperRight = Projected2D(center.x + right, center.y + top);
    auto lowerLeft = Projected2D(center.x + left, center.y + bottom);
    auto lowerRight = Projected2D(center.x + right, center.y + bottom);

    // Rotate
    upperLeft.rotate(center, -cameraOri.yaw);
    upperRight.rotate(center, -cameraOri.yaw);
    lowerLeft.rotate(center, -cameraOri.yaw);
    lowerRight.rotate(center, -cameraOri.yaw);

//    LOGD << "UL: " << upperLeft;
//    LOGD << "UR: " << upperRight;
//    LOGD << "LL: " << lowerLeft;
//    LOGD << "LR: " << lowerRight;

    // Convert to geographic
    auto ul = fromUTM(upperLeft, utmZone);
    auto ur = fromUTM(upperRight, utmZone);
    auto ll = fromUTM(lowerLeft, utmZone);
    auto lr = fromUTM(lowerRight, utmZone);

    geom.addPoint(ul.longitude, ul.latitude, groundHeight);
    geom.addPoint(ll.longitude, ll.latitude, groundHeight);
    geom.addPoint(lr.longitude, lr.latitude, groundHeight);
    geom.addPoint(ur.longitude, ur.latitude, groundHeight);
    geom.addPoint(ul.longitude, ul.latitude, groundHeight);
}

void Entry::toJSON(json &j) const{
    j["path"] = this->path;
    if (this->hash != "") j["hash"] = this->hash;
    j["type"] = this->type;
    if (!this->properties.empty()) j["properties"] = this->properties;
    j["mtime"] = this->mtime;
    j["size"] = this->size;
    j["depth"] = this->depth;

    if (!this->point_geom.empty()) j["point_geom"] = this->point_geom.toGeoJSON();
    if (!this->polygon_geom.empty()) j["polygon_geom"] = this->polygon_geom.toGeoJSON();

    if (!this->meta.empty()) j["meta"] = this->meta;
}

std::string Entry::toJSONString() const{
    json j;
    toJSON(j);
    std::stringstream s;
    s << j;
    return s.str();
}

void Entry::fromJSON(const json &j){
    j.at("path").get_to(this->path);
    if (j.contains("hash") && !j.at("hash").is_null()) j.at("hash").get_to(this->hash);
    j.at("type").get_to(this->type);
    j.at("size").get_to(this->size);
    j.at("depth").get_to(this->depth);
    j.at("mtime").get_to(this->mtime);

    // TODO: Add
    // j.at("properties").get_to(e.properties);
}

bool Entry::toGeoJSON(json &j, BasicGeometryType type){
    // Only export entries that have valid geometries
    std::vector<BasicGeometry *> geoms;
    if (!point_geom.empty() && (type == BasicGeometryType::BGAuto || type == BasicGeometryType::BGPoint)) geoms.push_back(&point_geom);
    if (!polygon_geom.empty() && (type == BasicGeometryType::BGAuto || type == BasicGeometryType::BGPolygon)) geoms.push_back(&polygon_geom);
    if (geoms.size() == 0) return false;

    json p;
    p["path"] = this->path;
    if (this->hash != "") p["hash"] = this->hash;
    p["type"] = this->type;
    p["mtime"] = this->mtime;
    p["size"] = this->size;

    // Populate properties
    for (json::iterator it = this->properties.begin(); it != this->properties.end(); ++it) {
        p[it.key()] = it.value();
    }

    // QGIS does not support GeometryCollections, so we only output the first
    // available geometry
    j = geoms[0]->toGeoJSON();
    j["properties"] = p;
    if (!this->meta.empty()) j["properties"]["meta"] = this->meta;

    return true;
}

std::string Entry::toString(){
    std::ostringstream s;
    s << "Path: " << this->path << "\n";
    if (this->hash != "") s << "SHA256: " << this->hash << "\n";
    s << "Type: " << typeToHuman(this->type) << " (" << this->type << ")" << "\n";

    for (json::iterator it = this->properties.begin(); it != this->properties.end(); ++it) {
        std::string k = it.key();
        if (k.length() > 0) k[0] = std::toupper(k[0]);

        if (k == "Bands"){
            s << k << ": " << it.value().size() << " [";
            for (json::iterator b = it.value().begin(); b != it.value().end(); b++){
                auto band = b.value();
                s << band["colorInterp"].get<std::string>() << "(" << band["type"].get<std::string>() << ")";
                if (b + 1 != it.value().end()) s << ",";
            }
            s << "]\n";
        }else{
            s << k << ": " << (it.value().is_string() ?
                                   it.value().get<std::string>() :
                                   it.value().dump()) << "\n";
        }
    }

    s << "Modified Time: " << this->mtime << "\n";
    s << "Size: " << io::bytesToHuman(this->size) << "\n";
    //s << "Tree Depth: " << this->depth << "\n";
    if (!this->point_geom.empty()) s << "Point Geometry: " << this->point_geom  << "\n";
    if (!this->polygon_geom.empty()) s << "Polygon Geometry: " << this->polygon_geom << "\n";
    if (!this->meta.empty()) s << "Meta: " << this->meta.dump(4) << "\n";
    return s.str();
}

void parseDroneDBEntry(const fs::path &ddbPath, Entry &entry){
    try{
        const auto db = ddb::open(ddbPath.string(), false);

        // The size of the database is the sum of all entries' sizes
        auto q = db->query("SELECT SUM(size) FROM entries");
        if (q->fetch()){
            entry.size = q->getInt64(0);
        }

        entry.properties = db->getProperties();
        entry.type = EntryType::DroneDB;
    }catch(AppException &e){
        LOGD << e.what();
        entry.type = EntryType::Directory;
    }
}

EntryType fingerprint(const fs::path &path){
    EntryType type = EntryType::Generic;
    io::Path p(path);

    // Check for markdown files
    if (p.checkExtension({"md"}))
        return EntryType::Markdown;

    // Check for point cloud files
    bool pointCloud = p.checkExtension({"laz", "las"});
    if (pointCloud)
        return EntryType::PointCloud;

    // Check for PLY files which could be a mesh or a point cloud
    if (p.checkExtension({"ply"})){
        return identifyPly(path);
    }

    // Check for 3D model files
    if (p.checkExtension({"obj"}))
        return EntryType::Model;

    // Check for vector files (geojson, dxf, dwg, shp, shz, fgb, topojson, kml, kmz, gpkg)
    if (p.checkExtension({"geojson", "dxf", "dwg", "shp", "shz", "fgb", "topojson", "kml", "kmz", "gpkg"}))
        return EntryType::Vector;

    // Check for image or video files
    bool jpg = p.checkExtension({"jpg", "jpeg"});
    bool dng = p.checkExtension({"dng"});
    bool tif = p.checkExtension({"tif", "tiff"});
    bool nongeoImage = p.checkExtension({"png", "gif"});
    bool video = p.checkExtension({"mp4", "mov", "avi", "mkv", "webm"});

    // Check if the file is a georeferenced raster (GeoTIFF)
    bool georaster = false;
    if (tif){
        GDALDatasetH hDataset = GDALOpen(p.string().c_str(), GA_ReadOnly);
        if(hDataset != NULL){
            const char *proj = GDALGetProjectionRef(hDataset);
            if (proj != NULL){
                georaster = std::string(proj) != "";
            }
            GDALClose(hDataset);
        } else {
            LOGD << "Cannot open " << p.string().c_str() << " for georaster test";
        }
    }

    // Determine if it's a regular image or a video
    bool image = (jpg || tif || dng || nongeoImage) && !georaster;

    if (image || video) {
        // Set as Image or Video type by default
        type = image ? EntryType::Image : EntryType::Video;

        try{
            auto image = Exiv2::ImageFactory::open(path.string());
            if (!image.get()) throw new IndexException("Cannot open " + path.string());

            image->readMetadata();
            ExifParser e(image.get());

            if (type == EntryType::Image){
                // Check if it's a panorama
                if (image->pixelWidth() / image->pixelHeight() >= 2)
                    type = EntryType::Panorama;
            }

            // Check for georeferencing tags
            if (e.hasTags()) {
                GeoLocation geo;
                if (e.extractGeo(geo)) {
                    if (type == EntryType::Image) type = EntryType::GeoImage;
                    else if (type == EntryType::Video) type = EntryType::GeoVideo;
                    else if (type == EntryType::Panorama) type = EntryType::GeoPanorama;
                }
            } else {
                LOGD << "No XMP/EXIF data found in " << path.string();
            }
        }catch(Exiv2::Error&){
            LOGD << "Cannot read EXIF data: " << path.string();
        }
    } else if (georaster) {
        type = EntryType::GeoRaster;
    }

    return type;
}



}
