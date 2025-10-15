/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "3d.h"

#include <regex>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "utils.h"

namespace ddb {
namespace {
// RAII helper class for automatic cleanup of temporary directories
class TempDirGuard {
    fs::path dir;
    bool shouldCleanup;

public:
    TempDirGuard() : shouldCleanup(false) {}

    void set(const fs::path& path) {
        dir = path;
        shouldCleanup = true;
    }

    ~TempDirGuard() {
        if (shouldCleanup && fs::exists(dir)) {
            LOGD << "Cleaning up temporary directory: " << dir;
            io::assureIsRemoved(dir);
        }
    }
};

// Check if file extension is GLTF or GLB
bool isGltfFile(const fs::path& filePath) {
    std::string ext = filePath.extension().string();
    utils::toLower(ext);
    return ext == ".gltf" || ext == ".glb";
}

// Convert GLTF/GLB to OBJ in a temporary directory
std::string convertGltfToObj(const std::string& inputGltf, TempDirGuard& tempGuard) {
    LOGD << "Input is GLTF/GLB, converting to OBJ first";

    fs::path inputPath(inputGltf);
    fs::path tempDir =
        fs::temp_directory_path() / ("ddb_gltf_convert_" + utils::generateRandomString(16));
    fs::create_directories(tempDir);
    tempGuard.set(tempDir);

    std::string outGeomPath;
    std::string outMtlPath;
    std::string outputBasePath = (tempDir / inputPath.stem()).string();

    convertGltfTo3dModel(inputGltf, outputBasePath, outGeomPath, outMtlPath, false, true);

    LOGD << "GLTF/GLB converted to: " << outGeomPath;
    return outGeomPath;
}

// Validate that all file dependencies exist
void validateDependencies(const std::string& objFile, const fs::path& parentPath) {
    auto deps = getObjDependencies(objFile);
    std::vector<std::string> missingDeps;

    for (const std::string& d : deps) {
        fs::path relPath = parentPath / d;
        if (!fs::exists(relPath))
            missingDeps.push_back(d);
    }

    if (!missingDeps.empty()) {
        std::string errorMessage = "Dependencies missing for " + objFile + ": ";
        for (size_t i = 0; i < missingDeps.size(); i++) {
            if (i > 0)
                errorMessage += ", ";
            errorMessage += missingDeps[i];
        }
        throw BuildDepMissingException(errorMessage, missingDeps);
    }
}

// Perform the actual nexus build
void performNexusBuild(const std::string& inputFile, const std::string& outputFile) {
#ifndef NO_NEXUS
    LOGD << "Building nexus file " << outputFile << " from " << inputFile;

    char errorMsg[2048] = {0};
    auto err = nexusBuild(inputFile.c_str(), outputFile.c_str(), errorMsg, sizeof(errorMsg));

    if (err == NXSERR_EXCEPTION) {
        std::string errorMessage = "Could not build nexus file for " + inputFile;
        if (errorMsg[0] != '\0') {
            errorMessage += ": " + std::string(errorMsg);
        }
        throw AppException(errorMessage);
    }
#else
    throw AppException("This version of ddb does not have the ability to generate Nexus files");
#endif
}
}  // namespace

std::string buildNexus(const std::string& inputObj, const std::string& outputNxs, bool overwrite) {
    TempDirGuard tempGuard;
    fs::path inputPath(inputObj);
    std::string actualInputObj = inputObj;

    // Convert GLTF/GLB to OBJ if needed
    if (isGltfFile(inputPath)) {
        actualInputObj = convertGltfToObj(inputObj, tempGuard);
        inputPath = fs::path(actualInputObj);
    }

    // Determine output file path
    const auto outFile =
        outputNxs.empty()
            ? inputPath.replace_filename(inputPath.filename().replace_extension(".nxz")).string()
            : outputNxs;

    // Check if output file already exists
    if (fs::exists(outFile)) {
        if (overwrite)
            io::assureIsRemoved(outFile);
        else
            throw AppException("File " + outFile + " already exists (delete it first)");
    }

    // Validate dependencies
    validateDependencies(actualInputObj, inputPath.parent_path());

    // Build the nexus file
    performNexusBuild(actualInputObj, outFile);

    return outFile;
}

std::optional<std::string> extractFileName(const std::string& input) {
    // Define the regex pattern for extracting file names without restricting file extension
    std::regex pattern("\"([^\"]+\\.[^\\s\"]+)\"|\\b([^\" \\t]+\\.[^\\s\"]+)\\b");

    std::smatch match;
    if (!std::regex_search(input, match, pattern))
        return std::nullopt;

    // Check which capturing group has been matched
    if (match[1].matched)
        return match[1].str();  // File name from double quotes

    if (match[2].matched)
        return match[2].str();  // Filenames not in quotes

    return std::nullopt;
}

std::vector<std::string> getObjDependencies(const std::string& obj) {
    std::vector<std::string> deps;
    if (!fs::exists(obj))
        throw FSException(obj + " does not exist");

    std::ifstream fin(obj);
    fs::path p(obj);
    fs::path parentPath = p.parent_path();

    const auto keys = {"map_Ka",
                       "map_Kd",
                       "map_Ks",
                       "map_Ns",
                       "map_d",
                       "disp",
                       "decal",
                       "bump",
                       "map_bump",
                       "refl",
                       "map_Pr",
                       "map_Pm",
                       "map_Ps",
                       "map_Ke"};

    std::string line;
    while (std::getline(fin, line)) {
        size_t mtllibPos = line.find("mtllib");

        // Parse the mtllib line, otherwise skip
        if (mtllibPos == std::string::npos)
            continue;

        // 6 = length of "mtllib"
        std::string mtlFile = line.substr(6 + mtllibPos, std::string::npos);
        utils::trim(mtlFile);

        // Remove double quotes if present
        if (mtlFile[0] == '"' && mtlFile[mtlFile.size() - 1] == '"')
            mtlFile = mtlFile.substr(1, mtlFile.size() - 2);

        deps.push_back(mtlFile);
        fs::path mtlRelPath = parentPath / mtlFile;

        if (!fs::exists(mtlRelPath))
            continue;

        // Parse MTL
        std::string mtlLine;
        std::ifstream mtlFin(mtlRelPath.string());
        while (std::getline(mtlFin, mtlLine)) {
            for (const auto& key : keys) {
                size_t keyPos = mtlLine.find(key);
                if (keyPos == std::string::npos)
                    continue;

                auto lineToParse = mtlLine.substr(keyPos + strlen(key), std::string::npos);
                auto textureFilename = extractFileName(lineToParse);

                if (textureFilename.has_value())
                    deps.push_back(textureFilename.value());
            }
        }
    }

    return deps;
}

}  // namespace ddb
