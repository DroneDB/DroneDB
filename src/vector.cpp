/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "vector.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"

namespace ddb{

void buildVector(const std::string &input, const std::string &outputVector){

    LOGD << "Building vector " << input << " to " << outputVector;

    if (!convertToGeoJSON(input, outputVector))
        throw AppException("Cannot convert " + input + " to GeoJSON");

}

bool convertToGeoJSON(const std::string& input, const std::string& output) {

    LOGD << "Converting " << input << " to GeoJSON";

    // Open the input dataset
    GDALDataset *inputDataset = (GDALDataset*) GDALOpenEx(input.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (inputDataset == nullptr) {
        LOGD << "Failed to open input file: " << input;
        return false;
    }

    // Define the GeoJSON driver
    GDALDriver *geoJsonDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    if (geoJsonDriver == nullptr) {
        LOGD << "GeoJSON driver not available";
        GDALClose(inputDataset);
        return false;
    }

    // Create the output GeoJSON dataset
    GDALDataset *outputDataset = geoJsonDriver->Create(output.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (outputDataset == nullptr) {
        LOGD << "Failed to create output file: " << output;
        GDALClose(inputDataset);
        return false;
    }

    int layers = inputDataset->GetLayerCount();

    LOGD << "Found " << layers << " layers in input file";

    // Loop through layers in the input dataset and copy them to the output dataset
    for (int i = 0; i < layers; i++) {
        OGRLayer *inputLayer = inputDataset->GetLayer(i);
        if (inputLayer == nullptr) {
            LOGD << "Failed to retrieve layer " << i << " from input file";
            continue;
        }

        LOGD << "Processing layer " << i << ": " << inputLayer->GetName();

        // Create a new layer in the output dataset with the same name and geometry type
        OGRLayer *outputLayer = outputDataset->CreateLayer(inputLayer->GetName(), nullptr, inputLayer->GetGeomType(), nullptr);
        if (outputLayer == nullptr) {
            LOGD << "Failed to create layer " << inputLayer->GetName() << " in output file";
            continue;
        }

        // Copy layer fields
        OGRFeatureDefn *inputLayerDefn = inputLayer->GetLayerDefn();
        for (int j = 0; j < inputLayerDefn->GetFieldCount(); j++) {
            OGRFieldDefn *fieldDefn = inputLayerDefn->GetFieldDefn(j);
            if (outputLayer->CreateField(fieldDefn) != OGRERR_NONE) {
                LOGD << "Failed to create field " << fieldDefn->GetNameRef() << " in output file";
            }
        }

        int features = inputLayer->GetFeatureCount();

        LOGD << "Found " << features << " features in layer";

        // Copy features
        OGRFeature *inputFeature;
        while ((inputFeature = inputLayer->GetNextFeature()) != nullptr) {
            OGRFeature *outputFeature = OGRFeature::CreateFeature(outputLayer->GetLayerDefn());
            if (outputFeature->SetGeometry(inputFeature->GetGeometryRef()) != OGRERR_NONE) {
                LOGD << "Failed to set geometry for feature in output file." << std::endl;
            }
            if (outputFeature->SetFrom(inputFeature) != OGRERR_NONE) {
                LOGD << "Failed to copy feature attributes." << std::endl;
            }
            if (outputLayer->CreateFeature(outputFeature) != OGRERR_NONE) {
                LOGD << "Failed to write feature to output file." << std::endl;
            }
            OGRFeature::DestroyFeature(outputFeature);
            OGRFeature::DestroyFeature(inputFeature);
        }
    }

    // Close datasets
    GDALClose(inputDataset);
    GDALClose(outputDataset);

    LOGD << "Conversion to GeoJSON completed: " << output << std::endl;
    return true;
}

}
