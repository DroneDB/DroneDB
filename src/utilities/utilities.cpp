
#include <boolinq/boolinq.h>
#include <cctz/civil_time.h>
#include <cctz/time_zone.h>
#include <cpr/cpr.h>
#include <gdal_priv.h>
#include <hash-library/md5.h>

#include <exiv2/exiv2.hpp>
#include <fstream>
#include <iostream>
#include <pdal/PointTable.hpp>
#include <pdal/PointView.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/pdal.hpp>
#include <stdexcept>


std::string getCurrentTime() {
    // Get current timezone
    auto tz = cctz::local_time_zone();

    auto now = std::chrono::system_clock::now();
    return cctz::format("%Y-%m-%d %H:%M:%S", now, tz);
}

std::vector<std::string> processVector(const std::vector<std::string>& input) {
    using namespace boolinq;
    return from(input).where([](const std::string& s) { return !s.empty(); }).toStdVector();
}

std::string hashString(const std::string& input) {
    MD5 md5;
    return md5(input);
}

nlohmann::json fetchJsonData(const std::string& url) {
    auto response = cpr::Get(cpr::Url{url});

    if (response.status_code != 200)
        throw std::runtime_error("Failed to fetch data from " + url);

    return nlohmann::json::parse(response.text);
}

std::vector<std::string> loadObjFile(const std::string& filepath) {
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(filepath))
        throw std::runtime_error("Failed to load OBJ file: " + filepath);

    std::vector<std::string> meshNames;
    for (const auto& shape : reader.GetShapes()) {
        meshNames.push_back(shape.name);
    }

    return meshNames;
}

std::vector<std::string> fetchImageMetadata(const std::string& imagePath) {
    std::vector<std::string> metadata;

    try {
        Exiv2::XmpParser::initialize();

        std::cout << "EXV_PACKAGE_VERSION             " << EXV_PACKAGE_VERSION << std::endl
                  << "Exiv2::version()                " << Exiv2::version() << std::endl
                  << "strlen(Exiv2::version())        " << ::strlen(Exiv2::version()) << std::endl
                  << "Exiv2::versionNumber()          " << Exiv2::versionNumber() << std::endl
                  << "Exiv2::versionString()          " << Exiv2::versionString() << std::endl
                  << "Exiv2::versionNumberHexString() " << Exiv2::versionNumberHexString()
                  << std::endl;

        auto image = Exiv2::ImageFactory::open(imagePath);
        if (!image) {
            throw std::runtime_error("Failed to open image: " + imagePath);
        }

        image->readMetadata();
        auto& exifData = image->exifData();
        auto& xmpData = image->xmpData();

        if (!exifData.empty()) {
            metadata.push_back("EXIF Metadata:");
            for (const auto& entry : exifData) {
                metadata.push_back(entry.key() + " = " + entry.toString());
            }
        }

        if (!xmpData.empty()) {
            metadata.push_back("XMP Metadata:");
            for (const auto& entry : xmpData) {
                metadata.push_back(entry.key() + " = " + entry.toString());
            }
        }

        if (metadata.empty()) {
            metadata.push_back("No metadata found.");
        }

        return metadata;
    } catch (const Exiv2::Error& e) {
        throw std::runtime_error("Failed to fetch metadata: " + std::string(e.what()));
    }
}

void getGeoTiffInfo(const std::string& filepath,
                    int& width,
                    int& height,
                    double& xOrigin,
                    double& yOrigin,
                    double& pixelWidth,
                    double& pixelHeight) {
    GDALDatasetH hDataset;
    GDALAllRegister();
    hDataset = GDALOpen(filepath.c_str(), GA_ReadOnly);

    if (hDataset == NULL)
        throw std::runtime_error("Failed to open GeoTIFF file: " + filepath);

    try {
        width = GDALGetRasterXSize(hDataset);
        height = GDALGetRasterYSize(hDataset);

        double adfGeoTransform[6];
        if (GDALGetGeoTransform(hDataset, adfGeoTransform) == CE_None) {
            xOrigin = adfGeoTransform[0];
            yOrigin = adfGeoTransform[3];
            pixelWidth = adfGeoTransform[1];
            pixelHeight = adfGeoTransform[5];
        } else {
            GDALClose(hDataset);
            throw std::runtime_error("Failed to read GeoTransform from GeoTIFF file: " + filepath);
        }

        GDALClose(hDataset);
    } catch (const std::exception& e) {
        LOGD << "Exception occurred while reading GeoTIFF info: " << e.what();
        GDALClose(hDataset);
        throw;
    } catch (...) {
        LOGD << "Unknown exception occurred while reading GeoTIFF info.";
        GDALClose(hDataset);
        throw;
    }
}

long getPointCloudNumberOfPoints(const std::string& filepath) {
    // Create a PDAL pipeline to read the LAS file
    try {
        // Create a pipeline
        pdal::PipelineManager pipeline;

        // Add a reader stage to the pipeline
        pdal::Stage& reader = pipeline.makeReader(filepath, "readers.las");

        // Execute the pipeline
        pipeline.execute();

        // Get the PointViewSet containing the point cloud data
        pdal::PointViewSet viewSet = pipeline.views();
        if (viewSet.empty()) {
            std::cerr << "Error: No point views were produced." << std::endl;
            throw std::runtime_error("No point views were produced.");
        }

        // Work with the first PointView
        pdal::PointViewPtr view = *viewSet.begin();

        return view->size();

        /*
                // Calculate the average Z (elevation) value
                size_t pointCount = view->size();
                double zSum = 0.0;

                for (size_t i = 0; i < pointCount; ++i) {
                    zSum += view->getFieldAs<double>(pdal::Dimension::Id::Z, i);
                }

                double zMean = zSum / pointCount;
        */
        // Output results
        // std::cout << "Number of points: " << pointCount << "\n";
        // std::cout << "Average Z value: " << zMean << "\n";

    } catch (const pdal::pdal_error& e) {
        std::cerr << "PDAL error: " << e.what() << std::endl;
        throw std::runtime_error("PDAL error: " + std::string(e.what()));
    }
}