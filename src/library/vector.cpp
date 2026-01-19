/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_inc.h"
#include "vector.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"

namespace ddb
{

    void buildVector(const std::string &input, const std::string &outputVector, bool overwrite)
    {
        fs::path p(input);

        auto ext = fs::path(input).extension().string();
        ddb::utils::toLower(ext);

        const auto outFile = outputVector.empty() ? p.replace_filename(p.filename().replace_extension(".fgb")).string() : outputVector;

        LOGD << "Building vector " << input << " to " << outputVector << " with overwrite " << (overwrite ? "true" : "false") << " and outFile " << outFile;

        // If it's already a flatgeobuf, we just copy it
        // TODO OPT: We should create a link instead of copying
        if (ext == ".fgb") {
            LOGD << "File is already a FlatGeobuf";

            if (overwrite)
            {
                LOGD << "Overwriting " << outputVector;
                io::assureIsRemoved(outputVector);
            }
            else if (fs::exists(outputVector))
            {
                LOGD << "Output vector already exists, nothing to do";
                return;
            }

            LOGD << "Copying " << input << " to " << outputVector;

            fs::copy_file(input, outputVector);

            return;
        }

        const auto deps = getVectorDependencies(input);
        std::vector<std::string> missingDeps;

        // Collect all missing dependencies
        for (const std::string &d : deps)
        {
            fs::path relPath = p.parent_path() / d;
            if (!fs::exists(relPath))
            {
                missingDeps.push_back(d);
            }
        }

        // If there are missing dependencies, throw exception with the complete list
        if (!missingDeps.empty())
        {
            std::string errorMessage = "Dependencies missing for " + input + ": ";
            for (size_t i = 0; i < missingDeps.size(); i++)
            {
                if (i > 0) errorMessage += ", ";
                errorMessage += missingDeps[i];
            }
            throw BuildDepMissingException(errorMessage, missingDeps);
        }

        if (!convertToFlatGeobuf(input, outFile))
            throw AppException("Cannot convert " + input + " to FlatGeobuf");
    }

    std::vector<std::string> getVectorDependencies(const std::string &input)
    {
        std::vector<std::string> deps;

        if (!fs::exists(input))
            throw FSException(input + " does not exist");

        auto ext = fs::path(input).extension().string();
        ddb::utils::toLower(ext);

        if (ext == ".shp")
        {
            deps.push_back(fs::path(input).replace_extension(".shx").filename().string());
        }
        else if (ext == ".shx")
        {
            deps.push_back(fs::path(input).replace_extension(".shp").filename().string());
        }

        return deps;

    }

    /**
 * Opens a GDAL dataset from an input file.
 *
 * @param input Path to the input file
 * @return Pointer to the GDAL dataset or nullptr on error
 */
GDALDatasetH openInputDataset(const char *input) {
    // Open the source dataset
    GDALDatasetH hSrcDS = GDALOpenEx(
        input,
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr);
    if (hSrcDS == nullptr) {
        LOGD << "Failed to open input dataset.";
    } else {
        LOGD << "Input dataset opened successfully.";
    }
    return hSrcDS;
}

/**
 * Performs direct conversion of a GDAL dataset to FlatGeobuf.
 *
 * @param hSrcDS Source dataset to convert
 * @param output Path to the output file
 * @param psOptions Conversion options
 * @return true if conversion succeeded, false otherwise
 */
bool performDirectConversion(GDALDatasetH hSrcDS, const char *output, GDALVectorTranslateOptions *psOptions) {
    LOGD << "Using direct conversion";

    // Perform the translation directly
    int pbUsageError = 0;
    GDALDatasetH hDstDS = GDALVectorTranslate(
        output,
        nullptr,
        1,
        &hSrcDS,
        psOptions,
        &pbUsageError);

    LOGD << "Direct translation completed.";

    if (hDstDS == nullptr || pbUsageError) {
        LOGD << "GDALVectorTranslate failed.";

        // Get last error
        const char *err = CPLGetLastErrorMsg();
        if (err != nullptr) {
            const auto num = CPLGetLastErrorNo();
            const auto type = CPLGetLastErrorType();
            LOGD << "Error " << num << " of type " << type << ": " << err;
        }

        return false;
    }

    LOGD << "Output dataset created successfully.";
    GDALClose(hDstDS);
    return true;
}

/**
 * Creates a temporary in-memory dataset for layer merging.
 *
 * @return Pointer to the temporary dataset or nullptr on error
 */
GDALDatasetH createTemporaryDataset() {
    GDALDriverH hDriver = GDALGetDriverByName("Memory");
    if (hDriver == nullptr) {
        LOGD << "Memory driver not available.";
        return nullptr;
    }

    GDALDatasetH hTempDS = GDALCreate(hDriver, "temp", 0, 0, 0, GDT_Unknown, nullptr);
    if (hTempDS == nullptr) {
        LOGD << "Failed to create temporary memory dataset.";
    }
    return hTempDS;
}

/**
 * Creates a unified layer in the temporary dataset.
 *
 * @param hTempDS Temporary dataset
 * @return Pointer to the created layer or nullptr on error
 */
OGRLayerH createMergedLayer(GDALDatasetH hTempDS) {
    // Create a single layer that will contain all features from all source layers
    // Use wkbUnknown to accept any type of geometry from source layers
    OGRLayerH mergedLayer = GDALDatasetCreateLayer(hTempDS, "merged", nullptr, wkbUnknown, nullptr);
    if (mergedLayer == nullptr) {
        LOGD << "Failed to create merged layer.";
        return nullptr;
    }

    // Add source_layer field to store the original layer name
    OGRFieldDefnH layerNameField = OGR_Fld_Create("source_layer", OFTString);
    OGR_L_CreateField(mergedLayer, layerNameField, FALSE);
    OGR_Fld_Destroy(layerNameField);

    return mergedLayer;
}

/**
 * Copies a single field from one feature to another, handling different data types.
 *
 * @param feature Source feature
 * @param newFeature Destination feature
 * @param fieldDefn Field definition to copy
 * @param srcIdx Field index in the source feature
 * @param targetIdx Field index in the destination feature
 */
void copyFeatureField(OGRFeatureH feature, OGRFeatureH newFeature, OGRFieldDefnH fieldDefn, int srcIdx, int targetIdx) {
    // Copy field value based on its type
    OGRFieldType fieldType = OGR_Fld_GetType(fieldDefn);
    switch (fieldType) {
        case OFTString:
            OGR_F_SetFieldString(newFeature, targetIdx, OGR_F_GetFieldAsString(feature, srcIdx));
            break;
        case OFTInteger:
            OGR_F_SetFieldInteger(newFeature, targetIdx, OGR_F_GetFieldAsInteger(feature, srcIdx));
            break;
        case OFTReal:
            OGR_F_SetFieldDouble(newFeature, targetIdx, OGR_F_GetFieldAsDouble(feature, srcIdx));
            break;
        case OFTDateTime:
            {
                int year, month, day, hour, minute, second, tzFlag;
                if (OGR_F_GetFieldAsDateTime(feature, srcIdx, &year, &month, &day, &hour, &minute, &second, &tzFlag)) {
                    OGR_F_SetFieldDateTime(newFeature, targetIdx, year, month, day, hour, minute, second, tzFlag);
                }
            }
            break;
        default:
            // For other types, use string representation
            OGR_F_SetFieldString(newFeature, targetIdx, OGR_F_GetFieldAsString(feature, srcIdx));
            break;
    }
}

/**
 * Copies field definitions from all source layers to the unified layer.
 *
 * @param hSrcDS Source dataset
 * @param layerCount Number of layers in the source dataset
 * @param mergedLayer Destination unified layer
 * @return Number of unique fields added
 */
int copyFieldDefinitions(GDALDatasetH hSrcDS, int layerCount, OGRLayerH mergedLayer) {
    // Add all fields from all layers (collect unique fields)
    std::set<std::string> uniqueFields;

    // First pass: collect all unique field names
    for (int i = 0; i < layerCount; i++) {
        OGRLayerH srcLayer = GDALDatasetGetLayer(hSrcDS, i);
        if (!srcLayer) continue;

        OGRFeatureDefnH defn = OGR_L_GetLayerDefn(srcLayer);
        int fieldCount = OGR_FD_GetFieldCount(defn);

        for (int j = 0; j < fieldCount; j++) {
            OGRFieldDefnH fieldDefn = OGR_FD_GetFieldDefn(defn, j);
            std::string fieldName = OGR_Fld_GetNameRef(fieldDefn);
            if (uniqueFields.find(fieldName) == uniqueFields.end()) {
                uniqueFields.insert(fieldName);
                // Create field in merged layer
                OGR_L_CreateField(mergedLayer, fieldDefn, FALSE);
            }
        }
    }

    LOGD << "Created merged layer with " << uniqueFields.size() << " unique fields";
    return uniqueFields.size();
}

/**
 * Copies all features from all source layers to the unified layer.
 *
 * @param hSrcDS Source dataset
 * @param layerCount Number of layers in the source dataset
 * @param mergedLayer Destination unified layer
 */
void copyFeaturesToMergedLayer(GDALDatasetH hSrcDS, int layerCount, OGRLayerH mergedLayer) {
    // Second pass: copy all features from all layers
    for (int i = 0; i < layerCount; i++) {
        OGRLayerH srcLayer = GDALDatasetGetLayer(hSrcDS, i);
        if (!srcLayer) continue;

        const char* layerName = OGR_L_GetName(srcLayer);
        LOGD << "Merging features from layer: " << layerName;

        // Get merged layer definition for creating new features
        OGRFeatureDefnH mergedDefn = OGR_L_GetLayerDefn(mergedLayer);

        // Copy all features
        OGR_L_ResetReading(srcLayer);
        OGRFeatureH feature;
        while ((feature = OGR_L_GetNextFeature(srcLayer)) != nullptr) {
            // Create new feature in merged layer
            OGRFeatureH newFeature = OGR_F_Create(mergedDefn);

            // Set the source layer name
            OGR_F_SetFieldString(newFeature, OGR_FD_GetFieldIndex(mergedDefn, "source_layer"), layerName);

            // Copy geometry
            OGRGeometryH geom = OGR_F_GetGeometryRef(feature);
            if (geom) {
                OGRGeometryH geomCopy = OGR_G_Clone(geom);
                OGR_F_SetGeometry(newFeature, geomCopy);
                OGR_G_DestroyGeometry(geomCopy);
            }

            // Copy all field values for fields that exist in both layers
            OGRFeatureDefnH srcDefn = OGR_L_GetLayerDefn(srcLayer);
            int fieldCount = OGR_FD_GetFieldCount(srcDefn);
            for (int j = 0; j < fieldCount; j++) {
                OGRFieldDefnH fieldDefn = OGR_FD_GetFieldDefn(srcDefn, j);
                const char* fieldName = OGR_Fld_GetNameRef(fieldDefn);

                int targetIdx = OGR_FD_GetFieldIndex(mergedDefn, fieldName);
                if (targetIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feature, j)) {
                    copyFeatureField(feature, newFeature, fieldDefn, j, targetIdx);
                }
            }

            // Add feature to merged layer
            OGR_L_CreateFeature(mergedLayer, newFeature);
            OGR_F_Destroy(newFeature);
            OGR_F_Destroy(feature);
        }
    }
}

/**
 * Creates a unified dataset from multiple layers and converts it to FlatGeobuf.
 *
 * @param hSrcDS Source dataset
 * @param layerCount Number of layers in the source dataset
 * @param output Path to the output file
 * @param psOptions Conversion options
 * @return true if conversion succeeded, false otherwise
 */
bool mergeLayersAndConvert(GDALDatasetH hSrcDS, int layerCount, const char *output, GDALVectorTranslateOptions *psOptions) {
    LOGD << "Source has multiple layers (" << layerCount << "), merging them into a single layer";

    // Create temporary dataset
    GDALDatasetH hTempDS = createTemporaryDataset();
    if (hTempDS == nullptr) {
        return false;
    }

    // Create merged layer
    OGRLayerH mergedLayer = createMergedLayer(hTempDS);
    if (mergedLayer == nullptr) {
        GDALClose(hTempDS);
        return false;
    }

    // Copy field definitions
    copyFieldDefinitions(hSrcDS, layerCount, mergedLayer);

    // Copy features
    copyFeaturesToMergedLayer(hSrcDS, layerCount, mergedLayer);

    // Now perform the translation from the temporary merged layer dataset to FlatGeobuf
    int pbUsageError = 0;
    GDALDatasetH hDstDS = GDALVectorTranslate(
        output,
        nullptr,
        1,
        &hTempDS,
        psOptions,
        &pbUsageError);

    LOGD << "Translation completed.";

    if (hDstDS == nullptr || pbUsageError) {
        LOGD << "GDALVectorTranslate failed.";

        // Get last error
        const char *err = CPLGetLastErrorMsg();
        if (err != nullptr) {
            const auto num = CPLGetLastErrorNo();
            const auto type = CPLGetLastErrorType();
            LOGD << "Error " << num << " of type " << type << ": " << err;
        }

        GDALClose(hTempDS);
        return false;
    }

    LOGD << "Output dataset created successfully.";
    GDALClose(hDstDS);
    GDALClose(hTempDS);
    return true;
}

/**
 * Internal function for converting a vector file to FlatGeobuf.
 *
 * @param hSrcDS Already opened GDAL dataset (caller retains ownership, this function does NOT close it)
 * @param output Path to the output file
 * @param argv Options for GDAL VectorTranslate
 * @return true if conversion succeeded, false otherwise
 */
bool convertToFlatGeobufInternal(GDALDatasetH hSrcDS, const char *output, char **argv) {
    if (hSrcDS == nullptr) {
        LOGD << "Source dataset is null.";
        return false;
    }

    // Get layer count from source dataset
    int layerCount = GDALDatasetGetLayerCount(hSrcDS);
    LOGD << "Source dataset has " << layerCount << " layers";

    // Parse options
    GDALVectorTranslateOptions *psOptions = GDALVectorTranslateOptionsNew(argv, nullptr);
    if (psOptions == nullptr) {
        LOGD << "Failed to create GDAL vector translate options.";
        return false;
    }

    bool result = false;

    // Convert based on layer count
    if (layerCount == 1) {
        // For single layer, use direct conversion
        result = performDirectConversion(hSrcDS, output, psOptions);
    } else {
        // For multiple layers, merge them first
        result = mergeLayersAndConvert(hSrcDS, layerCount, output, psOptions);
    }

    // Clean up options (dataset is managed by caller)
    GDALVectorTranslateOptionsFree(psOptions);

    return result;
}

/**
 * Attempts to convert a dataset to FlatGeobuf with automatic retry using PROMOTE_TO_MULTI on failure.
 *
 * @param hSrcDS Already opened GDAL dataset (caller retains ownership)
 * @param output Path to the output file
 * @param reproject If true, reproject to EPSG:4326
 * @param withSpatialIndex If true, create spatial index (SPATIAL_INDEX=YES)
 * @return true if conversion succeeded, false otherwise
 */
bool tryConvertWithFallback(GDALDatasetH hSrcDS, const char *output, bool reproject, bool withSpatialIndex) {
    // Build base arguments
    std::vector<char*> args;
    args.push_back(const_cast<char*>("-f"));
    args.push_back(const_cast<char*>("FlatGeobuf"));

    if (reproject) {
        args.push_back(const_cast<char*>("-t_srs"));
        args.push_back(const_cast<char*>("EPSG:4326"));
    }

    args.push_back(const_cast<char*>("-mapFieldType"));
    args.push_back(const_cast<char*>("StringList=String"));

    if (withSpatialIndex) {
        args.push_back(const_cast<char*>("-lco"));
        args.push_back(const_cast<char*>("SPATIAL_INDEX=YES"));
    }

    args.push_back(nullptr);

    // Try first conversion
    if (convertToFlatGeobufInternal(hSrcDS, output, args.data())) {
        return true;
    }

    LOGD << "Failed to convert to FlatGeobuf, retrying with PROMOTE_TO_MULTI";

    // Build retry arguments with PROMOTE_TO_MULTI
    std::vector<char*> argsMulti;
    argsMulti.push_back(const_cast<char*>("-f"));
    argsMulti.push_back(const_cast<char*>("FlatGeobuf"));

    if (reproject) {
        argsMulti.push_back(const_cast<char*>("-t_srs"));
        argsMulti.push_back(const_cast<char*>("EPSG:4326"));
    }

    argsMulti.push_back(const_cast<char*>("-mapFieldType"));
    argsMulti.push_back(const_cast<char*>("StringList=String"));

    if (withSpatialIndex) {
        argsMulti.push_back(const_cast<char*>("-lco"));
        argsMulti.push_back(const_cast<char*>("SPATIAL_INDEX=YES"));
    }

    argsMulti.push_back(const_cast<char*>("-nlt"));
    argsMulti.push_back(const_cast<char*>("PROMOTE_TO_MULTI"));
    argsMulti.push_back(nullptr);

    return convertToFlatGeobufInternal(hSrcDS, output, argsMulti.data());
}

/**
 * @brief Check if a GDAL dataset has a defined spatial reference system (CRS).
 *
 * Iterates through all layers in the dataset and checks if at least one
 * has a spatial reference system defined. This is useful to determine
 * whether reprojection is needed during format conversion.
 *
 * @param hDS Handle to an already opened GDAL dataset. Must not be nullptr.
 * @return true if at least one layer has a CRS defined, false otherwise.
 *
 * @note This overload does NOT close the dataset - caller retains ownership.
 * @see hasDefinedCRS(const std::string&) for file path version that manages dataset lifecycle.
 */
bool hasDefinedCRS(GDALDatasetH hDS) {
    if (hDS == nullptr) {
        return false;
    }

    bool hasCRS = false;
    int layerCount = GDALDatasetGetLayerCount(hDS);
    for (int i = 0; i < layerCount && !hasCRS; i++) {
        OGRLayerH hLayer = GDALDatasetGetLayer(hDS, i);
        if (hLayer != nullptr) {
            OGRSpatialReferenceH hSRS = OGR_L_GetSpatialRef(hLayer);
            if (hSRS != nullptr) {
                hasCRS = true;
                LOGD << "Layer " << i << " has CRS defined";
            }
        }
    }

    return hasCRS;
}

/**
 * @brief Check if a vector file has a defined spatial reference system (CRS).
 *
 * Opens the specified vector file and checks if at least one layer has a
 * spatial reference system defined. Files without a CRS are typically
 * assumed to be in WGS84 or have no georeferencing.
 *
 * @param input Path to the input vector file (e.g., SHP, GeoJSON, DXF).
 * @return true if at least one layer has a CRS defined, false if no CRS
 *         is found or if the file cannot be opened.
 *
 * @note This function opens and closes the dataset internally.
 * @see hasDefinedCRS(GDALDatasetH) for version that works with already opened datasets.
 */
bool hasDefinedCRS(const std::string &input) {
    GDALDatasetH hDS = GDALOpenEx(input.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (hDS == nullptr) {
        return false;
    }

    bool result = hasDefinedCRS(hDS);
    GDALClose(hDS);
    return result;
}

    bool convertToFlatGeobuf(const std::string &input, const std::string &output)
    {
        try
        {
            // Check if input and output strings are not empty
            if (input.empty())
            {
                LOGD << "Input filename is empty.";
                return false;
            }
            if (output.empty())
            {
                LOGD << "Output filename is empty.";
                return false;
            }

            // Check if input file exists
            if (!std::filesystem::exists(input))
            {
                LOGD << "Input file does not exist.";
                return false;
            }

            // Open dataset once and reuse for both CRS check and conversion
            GDALDatasetH hSrcDS = openInputDataset(input.c_str());
            if (hSrcDS == nullptr) {
                LOGD << "Failed to open input dataset.";
                return false;
            }

            // Check if source has a defined CRS - only reproject if it does
            // Files without CRS are assumed to be already in WGS84 or have no georeferencing
            bool needsReprojection = hasDefinedCRS(hSrcDS);
            LOGD << "Source has CRS: " << (needsReprojection ? "yes, will reproject to EPSG:4326" : "no, skipping reprojection");

            // GDAL VectorTranslate options explanation:
            // -f FlatGeobuf: Output format
            // -t_srs EPSG:4326: Reproject to WGS84 (only when source has CRS defined)
            //                   Required for proper display in web maps (OpenLayers, Leaflet, etc.)
            // -mapFieldType StringList=String: Convert string lists to simple strings for compatibility
            // -lco SPATIAL_INDEX=YES: Create R-tree index for efficient spatial queries via HTTP Range requests
            // -nlt PROMOTE_TO_MULTI: Promote geometries to multi-type (fallback for mixed geometry types)

            bool result = tryConvertWithFallback(hSrcDS, output.c_str(), needsReprojection, true);

            // Clean up dataset
            GDALClose(hSrcDS);

            return result;
        }
        catch (const std::exception &e)
        {
            LOGD << "Exception occurred during conversion to FlatGeobuf: " << e.what();

            return false;
        }
        catch (...)
        {
            LOGD << "An unknown exception occurred.";
            return false;
        }
    }

}
