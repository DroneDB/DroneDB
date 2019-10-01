#include <exiv2/exiv2.hpp>
#include "entry.h"

using json = nlohmann::json;

namespace ddb {

void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry) {
    // Parse file
    fs::path relPath = fs::relative(path, rootDirectory);
    entry.path = relPath.generic_string();
    entry.depth = utils::pathDepth(relPath);
    if (entry.mtime == 0) entry.mtime = utils::getModifiedTime(path);

    json meta;

    if (fs::is_directory(path)) {
        entry.type = Type::Directory;
        entry.hash = "";
        entry.size = 0;
    } else {
        if (entry.hash == "") entry.hash = Hash::ingest(path);
        entry.size = utils::getSize(path);

        entry.type = Type::Generic; // Default

        bool jpg = utils::checkExtension(path.extension(), {"jpg", "jpeg"});
        bool tif = utils::checkExtension(path.extension(), {"tif", "tiff"});

        // Images
        if (jpg || tif) {
            // TODO: if tif, check with GDAL if this is a georeferenced raster
            // for now, we don't allow tif
            if (jpg) {
                auto image = Exiv2::ImageFactory::open(path);
                if (!image.get()) throw new IndexException("Cannot open " + path.string());
                image->readMetadata();

                exif::Parser e(image.get());

                if (e.hasExif()) {
                    auto imageSize = e.extractImageSize();
                    meta["imageWidth"] = imageSize.width;
                    meta["imageHeight"] = imageSize.height;
                    meta["imageOrientation"] = e.extractImageOrientation();

                    meta["make"] = e.extractMake();
                    meta["model"] = e.extractModel();
                    meta["sensor"] = e.extractSensor();

                    auto sensorSize = e.extractSensorSize();
                    meta["sensorWidth"] = sensorSize.width;
                    meta["sensorHeight"] = sensorSize.height;

                    auto focal = e.computeFocal();
                    meta["focalLength"] = focal.length;
                    meta["focalLength35"] = focal.length35;
                    meta["captureTime"] = e.extractCaptureTime();

                    exif::CameraOrientation cameraOri;
                    bool hasCameraOri = e.extractCameraOrientation(cameraOri);
                    if (hasCameraOri) {
                        meta["cameraYaw"] = cameraOri.yaw;
                        meta["cameraPitch"] = cameraOri.pitch;
                        meta["cameraRoll"] = cameraOri.roll;
                    }

                    exif::GeoLocation geo;
                    if (e.extractGeo(geo)) {
                        entry.point_geom = utils::stringFormat("POINT Z (%f %f %f)", geo.longitude, geo.latitude, geo.altitude);
                        LOGV << "POINT GEOM: "<< entry.point_geom;

                        //e.printAllTags();

                        // Estimate image footprint
                        // TODO: if altitude is not known,
                        // we need to lookup an estimate from a DTM
                        // or set a default value
                        double relAltitude = e.extractRelAltitude();

                        if (hasCameraOri && relAltitude != 0.0 && sensorSize.width > 0.0) {
                            entry.polygon_geom = calculateFootprint(imageSize, sensorSize, geo, focal, cameraOri, relAltitude);
                        }

                        entry.type = Type::GeoImage;
                    } else {
                        // Not a georeferenced image, just a plain image
                        // do nothing
                    }
                } else {
                    LOGW << "No EXIF data found in " << path.string();
                }
            } else {
                LOGW << path.string() << " .tif file classified as generic";
            }
        }
    }

    // Serialize JSON
    entry.meta = meta.dump();
}

std::string calculateFootprint(const exif::ImageSize &imsize, const exif::SensorSize &sensorSize, const exif::GeoLocation &geo, const exif::Focal &focal, const exif::CameraOrientation &cameraOri, double relAltitude) {
    auto utmZone = geo::getUTMZone(geo.latitude, geo.longitude);
    auto center = geo::toUTM(geo.latitude, geo.longitude, utmZone);

    // TODO: implement!
    return "";

}

}

//                            if (i->key() == "Exif.GPSInfo.GPSLatitude") {
//                            std::cout << "===========" << std::endl;
//                            std::cout << std::setw(44) << std::setfill(' ') << std::left
//                                      << i->key() << " "
//                                      << "0x" << std::setw(4) << std::setfill('0') << std::right
//                                      << std::hex << i->tag() << " "
//                                      << std::setw(9) << std::setfill(' ') << std::left
//                                      << (tn ? tn : "Unknown") << " "
//                                      << std::dec << std::setw(3)
//                                      << std::setfill(' ') << std::right
//                                      << i->count() << "  "
//                                      << std::dec << i->value()
//                                      << "\n";
//                            }


