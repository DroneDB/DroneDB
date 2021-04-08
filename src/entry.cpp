/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <exiv2/exiv2.hpp>
#include "dbops.h"
#include "entry.h"

#include <ddb.h>

#include "mio.h"
#include "pointcloud.h"
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

        bool image = entry.type == EntryType::Image || entry.type == EntryType::GeoImage;
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
                    bool hasCameraOri = false;

                    auto imageSize = e.extractImageSize();
                    entry.meta["width"] = imageSize.width;
                    entry.meta["height"] = imageSize.height;
                    entry.meta["captureTime"] = e.extractCaptureTime();

                    if (image){
                        entry.meta["orientation"] = e.extractImageOrientation();
                        entry.meta["make"] = e.extractMake();
                        entry.meta["model"] = e.extractModel();
                        entry.meta["sensor"] = e.extractSensor();

                        if (e.extractSensorSize(sensorSize)){
                            entry.meta["sensorWidth"] = sensorSize.width;
                            entry.meta["sensorHeight"] = sensorSize.height;
                        }

                        if (e.computeFocal(focal)){
                            entry.meta["focalLength"] = focal.length;
                            entry.meta["focalLength35"] = focal.length35;
                        }

                        hasCameraOri = e.extractCameraOrientation(cameraOri);
                        if (hasCameraOri) {
                            entry.meta["cameraYaw"] = cameraOri.yaw;
                            entry.meta["cameraPitch"] = cameraOri.pitch;
                            entry.meta["cameraRoll"] = cameraOri.roll;
                            LOGD << "Camera Orientation: " << cameraOri;
                        }
                    }

                    GeoLocation geo;
                    if (e.extractGeo(geo)) {
                        entry.point_geom.addPoint(geo.longitude, geo.latitude, geo.altitude);
                        LOGD << "POINT GEOM: "<< entry.point_geom.toWkt();

                        //e.printAllTags();

                        // Estimate image footprint
                        if (image){
                            double relAltitude = 0.0;

                            if (hasCameraOri && e.extractRelAltitude(relAltitude) && sensorSize.width > 0.0 && focal.length > 0.0) {
                                calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, entry.polygon_geom);
                            }
                        }
                    }
                } else {
                    LOGD << "No XMP/EXIF data found in " << path.string();
                }
            }catch(Exiv2::AnyError&){
                LOGD << "Cannot read EXIF data: " << path.string();
            }
        }else if (entry.type == EntryType::GeoRaster){
            GDALDatasetH  hDataset;
            hDataset = GDALOpen( path.string().c_str(), GA_ReadOnly );
            int width = GDALGetRasterXSize(hDataset);
            int height = GDALGetRasterYSize(hDataset);

            entry.meta["width"] = width;
            entry.meta["height"] = height;

            double geotransform[6];
            if (GDALGetGeoTransform(hDataset, geotransform) == CE_None){
                entry.meta["geotransform"] = json::array();
                for (int i = 0; i < 6; i++) entry.meta["geotransform"].push_back(geotransform[i]);

                if (GDALGetProjectionRef(hDataset) != NULL){
                    std::string wkt = GDALGetProjectionRef(hDataset);
                    if (!wkt.empty()){
                        // Set projection
                        entry.meta["projection"] = wkt;

                        // Get lat/lon extent of raster
                        char *wktp = const_cast<char *>(wkt.c_str());
                        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
                        OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);

                        if (OSRImportFromWkt(hSrs, &wktp) != OGRERR_NONE){
                            throw GDALException("Cannot read spatial reference system for " + path.string() + ". Is PROJ available?");
                        }
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

            entry.meta["bands"] = json::array();
            for (int i = 0; i < GDALGetRasterCount(hDataset); i++){
                GDALRasterBandH hBand = GDALGetRasterBand(hDataset, i + 1);
                auto b = json::object();
                b["type"] = GDALGetDataTypeName(GDALGetRasterDataType(hBand));
                b["colorInterp"] = GDALGetColorInterpretationName(GDALGetRasterColorInterpretation(hBand));
                entry.meta["bands"].push_back(b);
            }
        }else if (entry.type == EntryType::PointCloud){
            PointCloudInfo info;
            if (getPointCloudInfo(path.string(), info)){
                entry.meta = info.toJSON();
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
    if (!this->meta.empty()) j["meta"] = this->meta;
    j["mtime"] = this->mtime;
    j["size"] = this->size;
    j["depth"] = this->depth;

    if (!this->point_geom.empty()) j["point_geom"] = this->point_geom.toGeoJSON();
    if (!this->polygon_geom.empty()) j["polygon_geom"] = this->polygon_geom.toGeoJSON();
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

    // Populate meta
    for (json::iterator it = this->meta.begin(); it != this->meta.end(); ++it) {
        p[it.key()] = it.value();
    }

    // QGIS does not support GeometryCollections, so we only output the first
    // available geometry
    j = geoms[0]->toGeoJSON();
    j["properties"] = p;

//  j["type"] = "Feature";
//  j["geometry"] = json({});
//  j["geometry"]["type"] = "GeometryCollection";
//  j["geometry"]["geometries"] = json::array();

//  for (auto &g : geoms){
//      j["geometry"]["geometries"] += g->toGeoJSON()["geometry"];
//  }

    return true;
}

std::string Entry::toString(){
    std::ostringstream s;
    s << "Path: " << this->path << "\n";
    if (this->hash != "") s << "SHA256: " << this->hash << "\n";
    s << "Type: " << typeToHuman(this->type) << " (" << this->type << ")" << "\n";

    for (json::iterator it = this->meta.begin(); it != this->meta.end(); ++it) {
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

    return s.str();
}

void parsePoint(BasicGeometry* point_geom, nlohmann::basic_json<>::value_type coordinates)
{
	if (coordinates.empty())
		throw DBException("Empty 'coordinates' field");

	if (coordinates.size() != 3)
		throw DBException(utils::stringFormat("Expected 3 coordinates but got ", coordinates.size()));

	const auto x = coordinates[0].get<double>();
	const auto y = coordinates[1].get<double>();
	const auto z = coordinates[2].get<double>();

	LOGD << "Parsed point: (" << x << "; " << y << "; " << z << ")";
	
	point_geom->addPoint(x, y, z);
}

void loadPointGeom(BasicPointGeometry *point_geom, const std::string& text){
    if (text.empty()) throw DBException("text is empty");
	
    if (point_geom == nullptr) throw DBException("point_geom is null");
	
    const auto j = json::parse(text);
	
    // {"type":"Point","coordinates":[-91.99456000000001,46.842607,198.31]}

    if (!j.contains("type")) throw DBException("Missing 'type' field");
    if (j["type"].get<std::string>() != "Point") throw DBException(utils::stringFormat("Cannot parse point_geom field: expected Point type but got: %s", j["type"].dump()));
    if (!j.contains("coordinates")) throw DBException("Missing 'coordinates' field");

	auto coordinates = j["coordinates"];

	parsePoint(point_geom, coordinates);

}

void loadPolygonGeom(BasicPolygonGeometry *polygon_geom, const std::string& text){
    if (text.empty()) throw DBException("text is empty");
    if (polygon_geom == nullptr) throw DBException("polygon_geom is null");

    const auto j = json::parse(text);

    if (!j.contains("type")) throw DBException("Missing 'type' field");
    if (j["type"].get<std::string>() != "Polygon") throw DBException(utils::stringFormat("Cannot parse polygon_geom field: expected Polygon type but got: %s", j["type"].dump()));
    if (!j.contains("coordinates")) throw DBException("Missing 'coordinates' field");

    auto coordinates = j["coordinates"];

    if (coordinates.empty()) throw DBException("Empty 'coordinates' field");
    if (coordinates.size() != 1) throw DBException(utils::stringFormat("Expected 1 coordinates but got ", coordinates.size()));

    coordinates = coordinates[0];

    if (coordinates.size() == 0) throw DBException("Expected coordinates but got 0");

    for (const auto &coord : coordinates){
        parsePoint(polygon_geom, coord);
	}
}

void parseDroneDBEntry(const fs::path &ddbPath, Entry &entry){
    try{
        const auto db = ddb::open(ddbPath.string(), false);

        // The size of the database is the sum of all entries' sizes
        auto q = db->query("SELECT SUM(size) FROM entries");
        if (q->fetch()){
            entry.size = q->getInt64(0);
        }

        entry.meta = db->getAttributes();
        entry.type = EntryType::DroneDB;
    }catch(AppException &e){
        LOGD << e.what();
        entry.type = EntryType::Directory;
    }
}

EntryType fingerprint(const fs::path &path){
    EntryType type = EntryType::Generic;
    io::Path p(path);

    bool markdown = p.checkExtension({"md"});

    if (markdown)
        return EntryType::Markdown;

    bool pointCloud = p.checkExtension({"laz", "las", "ply"}); // TODO: more?

    if (pointCloud)
        return EntryType::PointCloud;

    bool jpg = p.checkExtension({"jpg", "jpeg"});
    bool dng = p.checkExtension({"dng"});
    bool tif = p.checkExtension({"tif", "tiff"});
    bool nongeoImage = p.checkExtension({"png", "gif"});
    bool video = p.checkExtension({"mp4", "mov"});

    bool georaster = false;

    if (tif){
        GDALDatasetH  hDataset;
        hDataset = GDALOpen( p.string().c_str(), GA_ReadOnly );
        if( hDataset != NULL ){
            const char *proj = GDALGetProjectionRef(hDataset);
            if (proj != NULL){
                georaster = std::string(proj) != "";
            }
            GDALClose(hDataset);
        }else{
            LOGW << "Cannot open " << p.string().c_str() << " for georaster test";
        }
    }

    bool image = (jpg || tif || dng || nongeoImage) && !georaster;

    if (image || video) {
        // A normal image or video by default (refined later)
        type = image ? EntryType::Image : EntryType::Video;

        try{
            auto image = Exiv2::ImageFactory::open(path.string());
            if (!image.get()) throw new IndexException("Cannot open " + path.string());

            image->readMetadata();
            ExifParser e(image.get());

            if (e.hasTags()) {
                GeoLocation geo;
                if (e.extractGeo(geo)) {
                    if (type == EntryType::Image) type = EntryType::GeoImage;
                    else if (type == EntryType::Video) type = EntryType::GeoVideo;
                } else {
                    // Not a georeferenced image, just a plain image
                }
            } else {
                LOGD << "No XMP/EXIF data found in " << path.string();
            }
        }catch(Exiv2::AnyError&){
            LOGD << "Cannot read EXIF data: " << path.string();
        }
    }else if (georaster){
        type = EntryType::GeoRaster;
    }

    return type;
}



}
