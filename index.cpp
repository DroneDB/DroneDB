#include "index.h"
#include "exif.h"

void updateIndex(const std::string &directory, Database *db) {
    // fs::directory_options::skip_permission_denied
    for(auto i = fs::recursive_directory_iterator(directory);
            i != fs::recursive_directory_iterator();
            ++i ) {
        fs::path filename = i->path().filename();

        // Skip .ddb
        if(filename == ".ddb") i.disable_recursion_pending();

        else {
            if (checkExtension(i->path().extension(), {"jpg", "jpeg", "tif", "tiff"})) {
                std::cout << i->path() << '\n';

                std::string file = i->path().string();

                auto image = Exiv2::ImageFactory::open(file);
                if (!image.get()) throw new IndexException("Cannot open " + file);
                image->readMetadata();

                auto exifData = image->exifData();

                if (!exifData.empty()) {

                    exif::Parser p(exifData);

                    auto imageSize = p.extractImageSize();
                    LOGD << "Filename: " << file;
                    LOGD << "Image Size: " << imageSize.width << "x" << imageSize.height;
                    LOGD << "Make: " << p.extractMake();
                    LOGD << "Model: " << p.extractModel();
                    LOGD << "Sensor width: " << p.extractSensorWidth();
                    LOGD << "Sensor: " << p.extractSensor();
                    LOGD << "Focal35: " << p.computeFocal().f35;
                    LOGD << "FocalRatio: " << p.computeFocal().ratio;
                    LOGD << "Latitude: " << std::setprecision(14) << p.extractGeo().latitude;
                    LOGD << "Longitude: " << std::setprecision(14) << p.extractGeo().longitude;
                    LOGD << "Altitude: " << std::setprecision(14) << p.extractGeo().altitude;
                    LOGD << "Capture Time: " << p.extractCaptureTime();

                    exit(0);
                    Exiv2::ExifData::const_iterator end = exifData.end();
                    for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
                        const char* tn = i->typeName();
                        std::cout
                                << i->key() << " "

                                << i->value()
                                << " | " << tn
                                << "\n";

                        if (i->key() == "Exif.GPSInfo.GPSLatitude") {
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
                        }


                    }

                } else {
                    LOGW << "No EXIF data found in " << file;
                }

                auto xmpData = image->xmpData();
                if (!xmpData.empty()) {
                    for (Exiv2::XmpData::const_iterator md = xmpData.begin();
                            md != xmpData.end(); ++md) {
                        std::cout << std::setfill(' ') << std::left
                                  << std::setw(44)
                                  << md->key() << " "
                                  << std::setw(9) << std::setfill(' ') << std::left
                                  << md->typeName() << " "
                                  << std::dec << std::setw(3)
                                  << std::setfill(' ') << std::right
                                  << md->count() << "  "
                                  << std::dec << md->value()
                                  << std::endl;
                    }
                }

                exit(1);
            }
        }
    }
}

bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches) {
    std::string ext = extension.string();
    if (ext.size() < 1) return false;
    std::string extLowerCase = ext.substr(1, ext.size());
    utils::toLower(extLowerCase);

    for (auto &m : matches) {
        if (m == extLowerCase) return true;
    }
    return false;
}



