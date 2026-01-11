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

    /*
    TODO: Scrivere test per il build degli shapefile e di tutti gli altri
    Testare il nuovo sistema delle dipendenze
    Testare le interazioni con il sistema dei pending files
     */

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
 * Apre un dataset GDAL da un file di input.
 *
 * @param input Path del file di input
 * @return Puntatore al dataset GDAL o nullptr in caso di errore
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
 * Esegue la conversione diretta di un dataset GDAL a FlatGeobuf.
 *
 * @param hSrcDS Dataset sorgente da convertire
 * @param output Path del file di output
 * @param psOptions Opzioni per la conversione
 * @return true se la conversione è avvenuta con successo, false altrimenti
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
 * Crea un dataset temporaneo in memoria per la fusione dei layer.
 *
 * @return Puntatore al dataset temporaneo o nullptr in caso di errore
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
 * Crea un layer unificato nel dataset temporaneo.
 *
 * @param hTempDS Dataset temporaneo
 * @return Puntatore al layer creato o nullptr in caso di errore
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
 * Copia un singolo campo da una feature a un'altra, gestendo i diversi tipi di dati.
 *
 * @param feature Feature sorgente
 * @param newFeature Feature di destinazione
 * @param fieldDefn Definizione del campo da copiare
 * @param srcIdx Indice del campo nella feature sorgente
 * @param targetIdx Indice del campo nella feature di destinazione
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
 * Copia le definizioni dei campi da tutti i layer sorgenti al layer unificato.
 *
 * @param hSrcDS Dataset sorgente
 * @param layerCount Numero di layer nel dataset sorgente
 * @param mergedLayer Layer unificato di destinazione
 * @return Numero di campi unici aggiunti
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
 * Copia tutte le feature da tutti i layer sorgenti al layer unificato.
 *
 * @param hSrcDS Dataset sorgente
 * @param layerCount Numero di layer nel dataset sorgente
 * @param mergedLayer Layer unificato di destinazione
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
 * Crea dataset unificato dai layer multipli e lo converte in FlatGeobuf.
 *
 * @param hSrcDS Dataset sorgente
 * @param layerCount Numero di layer nel dataset sorgente
 * @param output Path del file di output
 * @param psOptions Opzioni per la conversione
 * @return true se la conversione è avvenuta con successo, false altrimenti
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
 * Funzione interna per la conversione di un file vettoriale a FlatGeobuf.
 *
 * @param input Path del file di input
 * @param output Path del file di output
 * @param argv Opzioni per GDAL VectorTranslate
 * @return true se la conversione è avvenuta con successo, false altrimenti
 */
bool convertToFlatGeobufInternal(const char *input, const char *output, char **argv) {
    // Open the source dataset
    GDALDatasetH hSrcDS = openInputDataset(input);
    if (hSrcDS == nullptr) {
        return false;
    }

    // Get layer count from source dataset
    int layerCount = GDALDatasetGetLayerCount(hSrcDS);
    LOGD << "Source dataset has " << layerCount << " layers";

    // Parse options
    GDALVectorTranslateOptions *psOptions = GDALVectorTranslateOptionsNew(argv, nullptr);
    if (psOptions == nullptr) {
        LOGD << "Failed to create GDAL vector translate options.";
        GDALClose(hSrcDS);
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

    // Clean up
    GDALVectorTranslateOptionsFree(psOptions);
    GDALClose(hSrcDS);

    return result;
}

    /**
     * Check if the input dataset has a defined spatial reference system (CRS).
     * Returns true if at least one layer has a CRS defined.
     */
    bool hasDefinedCRS(const std::string &input) {
        GDALDatasetH hDS = GDALOpenEx(input.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
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

        GDALClose(hDS);
        return hasCRS;
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

            // Check if source has a defined CRS - only reproject if it does
            // Files without CRS are assumed to be already in WGS84 or have no georeferencing
            bool needsReprojection = hasDefinedCRS(input);
            LOGD << "Source has CRS: " << (needsReprojection ? "yes, will reproject to EPSG:4326" : "no, skipping reprojection");

            if (needsReprojection) {
                // Prepare GDALVectorTranslate options with reprojection to WGS84
                // -t_srs EPSG:4326 ensures all output FGB files are in WGS84 coordinate system
                // This is required for proper display in web maps (OpenLayers, Leaflet, etc.)
                // -lco SPATIAL_INDEX=YES creates an R-tree index for efficient spatial queries via HTTP Range requests
                char *argv[] = {
                    const_cast<char *>("-f"), const_cast<char *>("FlatGeobuf"),
                    const_cast<char *>("-t_srs"), const_cast<char *>("EPSG:4326"),
                    const_cast<char *>("-mapFieldType"), const_cast<char *>("StringList=String"),
                    const_cast<char *>("-lco"), const_cast<char *>("SPATIAL_INDEX=YES"),
                    nullptr // Ensure null termination
                };

                if (!convertToFlatGeobufInternal(input.c_str(), output.c_str(), argv)) {
                    LOGD << "Failed to convert to FlatGeobuf, let's try with multipoligon";
                    char *argv[] = {
                        const_cast<char *>("-f"), const_cast<char *>("FlatGeobuf"),
                        const_cast<char *>("-t_srs"), const_cast<char *>("EPSG:4326"),
                        const_cast<char *>("-mapFieldType"), const_cast<char *>("StringList=String"),
                        const_cast<char *>("-lco"), const_cast<char *>("SPATIAL_INDEX=YES"),
                        const_cast<char *>("-nlt"), const_cast<char *>("PROMOTE_TO_MULTI"),
                        nullptr // Ensure null termination
                    };

                    return convertToFlatGeobufInternal(input.c_str(), output.c_str(), argv);
                }
            } else {
                // No CRS defined - convert without reprojection
                // -lco SPATIAL_INDEX=YES creates an R-tree index for efficient spatial queries via HTTP Range requests
                char *argv[] = {
                    const_cast<char *>("-f"), const_cast<char *>("FlatGeobuf"),
                    const_cast<char *>("-mapFieldType"), const_cast<char *>("StringList=String"),
                    const_cast<char *>("-lco"), const_cast<char *>("SPATIAL_INDEX=YES"),
                    nullptr // Ensure null termination
                };

                if (!convertToFlatGeobufInternal(input.c_str(), output.c_str(), argv)) {
                    LOGD << "Failed to convert to FlatGeobuf, let's try with multipoligon";
                    char *argv[] = {
                        const_cast<char *>("-f"), const_cast<char *>("FlatGeobuf"),
                        const_cast<char *>("-mapFieldType"), const_cast<char *>("StringList=String"),
                        const_cast<char *>("-lco"), const_cast<char *>("SPATIAL_INDEX=YES"),
                        const_cast<char *>("-nlt"), const_cast<char *>("PROMOTE_TO_MULTI"),
                        nullptr // Ensure null termination
                    };

                    return convertToFlatGeobufInternal(input.c_str(), output.c_str(), argv);
                }
            }

            return true;

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
