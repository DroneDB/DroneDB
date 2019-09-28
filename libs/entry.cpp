#include <exiv2/exiv2.hpp>
#include "entry.h"

using json = nlohmann::json;

namespace ddb {

void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry) {
    // Parse file
    entry.path = fs::relative(path, rootDirectory).generic_string();
    json meta;

    if (fs::is_directory(path)) {
        entry.type = Type::Directory;
        entry.mtime = 0;
        entry.hash = "";
        entry.size = 0;
    } else {
        if (entry.mtime == 0) entry.mtime = utils::getModifiedTime(path);
        if (entry.hash == "") entry.hash = Hash::ingest(path);
        entry.size = utils::getSize(path);

        entry.type = Type::Generic; // Default

        // Images
        if (utils::checkExtension(path.extension(), {"jpg", "jpeg", "tif", "tiff"})) {
            auto image = Exiv2::ImageFactory::open(path);
            if (!image.get()) throw new IndexException("Cannot open " + path.string());
            image->readMetadata();

            auto exifData = image->exifData();

            if (!exifData.empty()) {

                exif::Parser p(exifData);

                auto imageSize = p.extractImageSize();
                meta["imageWidth"] = imageSize.width;
                meta["imageHeight"] = imageSize.height;
                meta["make"] = p.extractMake();
                meta["model"] = p.extractModel();
                meta["sensorWidth"] = p.extractSensorWidth();
                meta["sensor"] = p.extractSensor();

                auto focal = p.computeFocal();
                meta["focal35"] = focal.f35;
                meta["focalRatio"] = focal.ratio;

                auto geo = p.extractGeo();
//                meta["latitude"]
                // TODO!

                LOGD << "Latitude: " << std::setprecision(14) << p.extractGeo().latitude;
                LOGD << "Longitude: " << std::setprecision(14) << p.extractGeo().longitude;
                LOGD << "Altitude: " << std::setprecision(14) << p.extractGeo().altitude;
                LOGD << "Capture Time: " << p.extractCaptureTime();
                LOGD << "Orientation: " << p.extractOrientation();


                entry.type = Type::GeoImage;

                // TODO: meta

            } else {
                LOGW << "No EXIF data found in " << path.string();
            }
        }
    }

    // Serialize JSON
    entry.meta = meta.dump();
}

}


//                        Exiv2::ExifData::const_iterator end = exifData.end();
//                        for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
//                            const char* tn = i->typeName();
//                            std::cout
//                                    << i->key() << " "

//                                    << i->value()
//                                    << " | " << tn
//                                    << "\n";

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

//                auto xmpData = image->xmpData();
//                if (!xmpData.empty()) {
//                    for (Exiv2::XmpData::const_iterator md = xmpData.begin();
//                            md != xmpData.end(); ++md) {
//                        std::cout << std::setfill(' ') << std::left
//                                  << std::setw(44)
//                                  << md->key() << " "
//                                  << std::setw(9) << std::setfill(' ') << std::left
//                                  << md->typeName() << " "
//                                  << std::dec << std::setw(3)
//                                  << std::setfill(' ') << std::right
//                                  << md->count() << "  "
//                                  << std::dec << md->value()
//                                  << std::endl;
//                    }
//                }
