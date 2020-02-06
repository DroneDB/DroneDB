#include <exiv2/exiv2.hpp>
#include "entry.h"
#include "gdal_priv.h"

namespace entry {

bool parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool computeHash) {
    if (!fs::exists(path)) return false;

    // Parse file
    fs::path relPath = fs::relative(path, rootDirectory);
    entry.path = relPath.generic_string();
    entry.depth = utils::pathDepth(relPath);
    if (entry.mtime == 0) entry.mtime = utils::getModifiedTime(path);

    if (fs::is_directory(path)) {
        entry.type = Type::Directory;
        entry.hash = "";
        entry.size = 0;
    } else {
        if (entry.hash == "" && computeHash) entry.hash = Hash::ingest(path);
        entry.size = utils::getSize(path);

        entry.type = Type::Generic; // Default

        bool jpg = utils::checkExtension(path.extension(), {"jpg", "jpeg"});
        bool tif = utils::checkExtension(path.extension(), {"tif", "tiff"});
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

        bool image = (jpg || tif) && !georaster;

        if (image) {
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

    LOGD << "xView: " << utils::rad2deg(xView);
    LOGD << "yView: " << utils::rad2deg(yView);
    LOGD << "bottom: " << bottom;
    LOGD << "top: " << top;
    LOGD << "left: " << left;
    LOGD << "right: " << right;

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

    LOGD << "UL: " << upperLeft;
    LOGD << "UR: " << upperRight;
    LOGD << "LL: " << lowerLeft;
    LOGD << "LR: " << lowerRight;

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

}



