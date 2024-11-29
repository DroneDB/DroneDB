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

    if (!convertToFlatGeobuf(input, outputVector))
        throw AppException("Cannot convert " + input + " to FlatGeobuf");

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

        // Open the source dataset
        GDALDatasetH hSrcDS = GDALOpenEx(
            input.c_str(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr);
        if (hSrcDS == nullptr)
        {
            LOGD << "Failed to open input dataset.";
            return false;
        }

        LOGD << "Input dataset opened successfully.";

        // Prepare GDALVectorTranslate options
        char *argv[] = {
            const_cast<char *>("-f"), const_cast<char *>("FlatGeobuf"),
            const_cast<char *>("-mapFieldType"), const_cast<char *>("StringList=String"),
            nullptr // Ensure null termination
        };

        // Parse options
        GDALVectorTranslateOptions *psOptions = GDALVectorTranslateOptionsNew(argv, nullptr);
        if (psOptions == nullptr)
        {
            LOGD << "Failed to create GDAL vector translate options.";
            GDALClose(hSrcDS);
            return false;
        }

        // Perform the translation
        int pbUsageError = 0;
        GDALDatasetH hDstDS = GDALVectorTranslate(
            output.c_str(),
            nullptr,
            1,
            &hSrcDS,
            psOptions,
            &pbUsageError);

        LOGD << "Translation completed.";

        if (hDstDS == nullptr || pbUsageError)
        {
            LOGD << "GDALVectorTranslate failed.";
            GDALVectorTranslateOptionsFree(psOptions);
            GDALClose(hSrcDS);
            return false;
        }

        LOGD << "Output dataset created successfully.";

        // Clean up
        GDALVectorTranslateOptionsFree(psOptions);
        GDALClose(hDstDS);
        GDALClose(hSrcDS);

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
