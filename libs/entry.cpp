#include <exiv2/exiv2.hpp>
#include "entry.h"
#include "gdal_priv.h"

namespace entry {

using namespace ddb;

bool parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, ParseEntryOpts &opts) {
    if (!fs::exists(path)){
        if (opts.stopOnError) throw FSException(path.string() + " does not exist");
        entry.type = Type::Undefined;
        return false;
    }

    // Parse file
    fs::path relPath = fs::weakly_canonical(fs::absolute(path).lexically_relative(fs::absolute(rootDirectory)));
    entry.path = relPath.generic_string();
    entry.depth = utils::pathDepth(relPath);
    if (entry.mtime == 0) entry.mtime = utils::getModifiedTime(path);

    if (fs::is_directory(path)) {
        entry.type = Type::Directory;
        entry.hash = "";
        entry.size = 0;

        // Check for DroneDB dir
        try{
            if (fs::exists(path / ".ddb" / "dbase.sqlite")){
                entry.type = Type::DroneDB;
            }
        }catch(const fs::filesystem_error &e){
            LOGD << "Cannot check " << path.string() << " .ddb presence: " << e.what();
        }
    } else {
        if (entry.hash == "" && opts.withHash) entry.hash = Hash::fileSHA256(path);
        entry.size = utils::getSize(path);

        entry.type = Type::Generic; // Default

        bool jpg = utils::checkExtension(path.extension(), {"jpg", "jpeg"});
        bool tif = utils::checkExtension(path.extension(), {"tif", "tiff"});
        bool nongeoImage = utils::checkExtension(path.extension(), {"png", "gif"});

        bool georaster = false;

        if (tif){
            GDALDatasetH  hDataset;
            hDataset = GDALOpen( path.c_str(), GA_ReadOnly );
            if( hDataset != NULL ){
                const char *proj = GDALGetProjectionRef(hDataset);
                if (proj != NULL){
                    georaster = std::string(proj) != "";
                }
                GDALClose(hDataset);
            }else{
                LOGW << "Cannot open " << path.c_str() << " for georaster test";
            }
        }

        bool image = (jpg || tif || nongeoImage) && !georaster;

        if (image) {
            // A normal image by default (refined later)
            entry.type = Type::Image;

            try{
                auto image = Exiv2::ImageFactory::open(path);
                if (!image.get()) throw new IndexException("Cannot open " + path.string());

                image->readMetadata();
                exif::Parser e(image.get());

                if (e.hasExif()) {
                    auto imageSize = e.extractImageSize();
                    entry.meta["imageWidth"] = imageSize.width;
                    entry.meta["imageHeight"] = imageSize.height;
                    entry.meta["imageOrientation"] = e.extractImageOrientation();

                    entry.meta["make"] = e.extractMake();
                    entry.meta["model"] = e.extractModel();
                    entry.meta["sensor"] = e.extractSensor();

                    exif::SensorSize sensorSize;
                    if (e.extractSensorSize(sensorSize)){
                        entry.meta["sensorWidth"] = sensorSize.width;
                        entry.meta["sensorHeight"] = sensorSize.height;
                    }

                    exif::Focal focal;
                    if (e.computeFocal(focal)){
                        entry.meta["focalLength"] = focal.length;
                        entry.meta["focalLength35"] = focal.length35;
                    }
                    entry.meta["captureTime"] = e.extractCaptureTime();

                    exif::CameraOrientation cameraOri;
                    bool hasCameraOri = e.extractCameraOrientation(cameraOri);
                    if (hasCameraOri) {
                        entry.meta["cameraYaw"] = cameraOri.yaw;
                        entry.meta["cameraPitch"] = cameraOri.pitch;
                        entry.meta["cameraRoll"] = cameraOri.roll;
                        LOGD << "Camera Orientation: " << cameraOri;
                    }

                    exif::GeoLocation geo;
                    if (e.extractGeo(geo)) {
                        entry.point_geom.addPoint(geo.longitude, geo.latitude, geo.altitude);
                        LOGD << "POINT GEOM: "<< entry.point_geom;

                        //e.printAllTags();

                        // Estimate image footprint
                        double relAltitude = 0.0;

                        if (hasCameraOri && e.extractRelAltitude(relAltitude) && sensorSize.width > 0.0 && focal.length > 0.0) {
                            calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, entry.polygon_geom);
                        }

                        entry.type = Type::GeoImage;
                    } else {
                        // Not a georeferenced image, just a plain image
                        // do nothing
                    }
                } else {
                    LOGD << "No EXIF data found in " << path.string();
                }
            }catch(Exiv2::AnyError& e){
                LOGD << "Cannot read EXIF data: " << path.string();
                if (opts.stopOnError) throw FSException("Cannot read EXIF data: " + path.string());
            }
        }else if (georaster){
            entry.type = Type::GeoRaster;

            // TODO: fill entries
        }
    }

    return true;
}

// Adapted from https://github.com/mountainunicycler/dronecamerafov/tree/master
void calculateFootprint(const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom) {
    auto utmZone = geo::getUTMZone(geo.latitude, geo.longitude);
    auto center = geo::toUTM(geo.latitude, geo.longitude, utmZone);
    double groundHeight = geo.altitude != 0.0 ? geo.altitude - relAltitude : relAltitude;

    // Field of view

    // Wide
    double xView = 2.0 * atan(sensorSize.width / (2.0 * focal.length));

    // Tall
    double yView = 2.0 * atan(sensorSize.height / (2.0 * focal.length));

    // Cap pitch to 30 degrees
    double pitch = cameraOri.pitch;
    if (pitch > -60){
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
    auto upperLeft = geo::Projected2D(center.x + left, center.y + top);
    auto upperRight = geo::Projected2D(center.x + right, center.y + top);
    auto lowerLeft = geo::Projected2D(center.x + left, center.y + bottom);
    auto lowerRight = geo::Projected2D(center.x + right, center.y + bottom);

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
    auto ul = geo::fromUTM(upperLeft, utmZone);
    auto ur = geo::fromUTM(upperRight, utmZone);
    auto ll = geo::fromUTM(lowerLeft, utmZone);
    auto lr = geo::fromUTM(lowerRight, utmZone);

    geom.addPoint(ul.longitude, ul.latitude, groundHeight);
    geom.addPoint(ll.longitude, ll.latitude, groundHeight);
    geom.addPoint(lr.longitude, lr.latitude, groundHeight);
    geom.addPoint(ur.longitude, ur.latitude, groundHeight);
    geom.addPoint(ul.longitude, ul.latitude, groundHeight);
}

std::string BasicPointGeometry::toWkt() const{
    if (empty()) return "";
    return utils::stringFormat("POINT Z (%lf %lf %lf)", points[0].x, points[0].y, points[0].z);
}

json BasicPointGeometry::toGeoJSON() const{
    json j;
    initGeoJsonBase(j);
    j["geometry"]["type"] = "Point";
    j["geometry"]["coordinates"] = json::array();

    if (!empty()){
        j["geometry"]["coordinates"] += points[0].x;
        j["geometry"]["coordinates"] += points[0].y;
        j["geometry"]["coordinates"] += points[0].z;
    }
    return j;
}

std::string BasicPolygonGeometry::toWkt() const{
    if (empty()) return "";

    std::ostringstream os;
    os << "POLYGONZ ((";
    bool first = true;
    for (auto &p : points){
        if (!first) os << ", ";
        os << p.x << " " << p.y << " " << p.z;
        first = false;
    }
    os << "))";
    return os.str();
}

json BasicPolygonGeometry::toGeoJSON() const{
    json j;
    initGeoJsonBase(j);
    j["geometry"]["type"] = "Polygon";
    j["geometry"]["coordinates"] = json::array();

    for (auto &p : points){
        json c = json::array();
        c += p.x;
        c += p.y;
        c += p.z;

        j["geometry"]["coordinates"] += c;
    }
    return j;
}

void BasicGeometry::addPoint(const Point &p){
    points.push_back(p);
}

void BasicGeometry::addPoint(double x, double y, double z){
    points.push_back(Point(x, y, z));
}

Point BasicGeometry::getPoint(int index){
    if (index >= static_cast<int>(points.size())) throw AppException("Out of bounds exception");
    return points[index];
}

bool BasicGeometry::empty() const{
    return points.empty();
}

int BasicGeometry::size() const{
    return points.size();
}

void BasicGeometry::initGeoJsonBase(json &j) const{
    j["type"] = "Feature";
    j["geometry"] = json({});
    j["properties"] = json({});
}

void Entry::toJSON(json &j){
    j["path"] = this->path;
    if (this->hash != "") j["hash"] = this->hash;
    j["type"] = this->type;
    if (!this->meta.empty()) j["meta"] = this->meta;
    j["mtime"] = this->mtime;
    j["size"] = this->size;
    //j["depth"] = this->depth;

    if (!this->point_geom.empty()) j["point_geom"] = this->point_geom.toGeoJSON();
    if (!this->polygon_geom.empty()) j["polygon_geom"] = this->polygon_geom.toGeoJSON();
}

bool Entry::toGeoJSON(json &j){
    // Only export entries that have valid geometries
    std::vector<BasicGeometry *> geoms;
    if (!point_geom.empty()) geoms.push_back(&point_geom);
    if (!polygon_geom.empty()) geoms.push_back(&polygon_geom);
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

        s << k << ": " << (it.value().is_string() ?
                               it.value().get<std::string>() :
                               it.value().dump()) << "\n";
    }

    s << "Modified Time: " << this->mtime << "\n";
    s << "Size: " << utils::bytesToHuman(this->size) << "\n";
    //s << "Tree Depth: " << this->depth << "\n";
    if (!this->point_geom.empty()) s << "Point Geometry: " << this->point_geom  << "\n";
    if (!this->polygon_geom.empty()) s << "Polygon Geometry: " << this->polygon_geom << "\n";

    return s.str();
}

}



