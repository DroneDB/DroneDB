/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "3d.h"

#include <fstream>
#include <optional>
#include <regex>

#include "exceptions.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "obj2tiles_runner.h"
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

// Check if file extension is GLB (binary format with embedded resources)
bool isGlbFile(const fs::path& filePath) {
    std::string ext = filePath.extension().string();
    utils::toLower(ext);
    return ext == ".glb";
}

// Check if file extension is PLY
bool isPlyFile(const fs::path& filePath) {
    std::string ext = filePath.extension().string();
    utils::toLower(ext);
    return ext == ".ply";
}

// Helper to check if a URI is a data URI (embedded base64 content)
bool isDataUri(const std::string& uri) {
    return uri.find("data:") == 0;
}

// Helper to check if a URI is absolute (external resource)
bool isAbsoluteUri(const std::string& uri) {
    return uri.find("http://") == 0 || uri.find("https://") == 0 || uri.find("file://") == 0;
}

// Helper to validate and sanitize a relative path (prevent path traversal attacks)
bool isSafePath(const std::string& path, const fs::path& parentPath) {
    try {
        // Resolve the path relative to parent
        fs::path resolvedPath = (parentPath / path).lexically_normal();
        fs::path canonicalParent = parentPath.lexically_normal();

        // Check if resolved path is within parent directory
        // This prevents path traversal like "../../../etc/passwd"
        auto [rootEnd, nothing] = std::mismatch(
            canonicalParent.begin(), canonicalParent.end(),
            resolvedPath.begin(), resolvedPath.end()
        );

        return rootEnd == canonicalParent.end();
    } catch (...) {
        return false;
    }
}

// Parse GLTF JSON and extract dependencies (buffers and images)
std::vector<std::string> parseGltfJson(const json& gltfJson, const fs::path& parentPath) {
    std::vector<std::string> deps;

    try {
        // Extract buffer URIs
        if (gltfJson.contains("buffers") && gltfJson["buffers"].is_array()) {
            for (const auto& buffer : gltfJson["buffers"]) {
                if (buffer.contains("uri") && buffer["uri"].is_string()) {
                    std::string uri = buffer["uri"];

                    // Skip data URIs and absolute URIs
                    if (isDataUri(uri) || isAbsoluteUri(uri))
                        continue;

                    // Validate path safety
                    if (!isSafePath(uri, parentPath)) {
                        LOGW << "Skipping unsafe buffer path: " << uri;
                        continue;
                    }

                    deps.push_back(uri);
                }
            }
        }

        // Extract image URIs
        if (gltfJson.contains("images") && gltfJson["images"].is_array()) {
            for (const auto& image : gltfJson["images"]) {
                if (image.contains("uri") && image["uri"].is_string()) {
                    std::string uri = image["uri"];

                    // Skip data URIs and absolute URIs
                    if (isDataUri(uri) || isAbsoluteUri(uri))
                        continue;

                    // Validate path safety
                    if (!isSafePath(uri, parentPath)) {
                        LOGW << "Skipping unsafe image path: " << uri;
                        continue;
                    }

                    deps.push_back(uri);
                }
            }
        }
    } catch (const json::exception& e) {
        throw AppException("Error parsing GLTF JSON: " + std::string(e.what()));
    }

    return deps;
}

// Read and parse GLB file to extract JSON chunk
json readGlbJson(const std::string& glbPath) {
    std::ifstream file(glbPath, std::ios::binary);
    if (!file.is_open())
        throw FSException("Cannot open GLB file: " + glbPath);

    // GLB Header structure (12 bytes)
    struct GLBHeader {
        uint32_t magic;    // 0x46546C67 ("glTF")
        uint32_t version;  // 2
        uint32_t length;   // Total file length
    } header;

    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file.good())
        throw AppException("Invalid GLB file: cannot read header");

    // Verify magic number (0x46546C67 = "glTF" in ASCII)
    if (header.magic != 0x46546C67)
        throw AppException("Invalid GLB file: incorrect magic number");

    // Verify version (should be 2)
    if (header.version != 2)
        throw AppException("Unsupported GLB version: " + std::to_string(header.version));

    // GLB Chunk structure
    struct GLBChunk {
        uint32_t chunkLength;
        uint32_t chunkType;
    } chunk;

    file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
    if (!file.good())
        throw AppException("Invalid GLB file: cannot read chunk header");

    // First chunk should be JSON (type 0x4E4F534A)
    if (chunk.chunkType != 0x4E4F534A)
        throw AppException("Invalid GLB file: first chunk is not JSON");

    // Read JSON data
    std::vector<char> jsonData(chunk.chunkLength);
    file.read(jsonData.data(), chunk.chunkLength);
    if (!file.good())
        throw AppException("Invalid GLB file: cannot read JSON chunk");

    // Parse JSON
    try {
        return json::parse(jsonData.begin(), jsonData.end());
    } catch (const json::exception& e) {
        throw AppException("Invalid GLB file: JSON parse error: " + std::string(e.what()));
    }
}

// Validate GLTF dependencies
void validateGltfDependencies(const std::string& gltfFile, const fs::path& parentPath) {
    // GLB files have everything embedded, skip validation
    if (isGlbFile(gltfFile)) {
        LOGD << "GLB file detected, skipping dependency validation (embedded resources)";
        return;
    }

    auto deps = getGltfDependencies(gltfFile);
    std::vector<std::string> missingDeps;

    for (const std::string& d : deps) {
        fs::path relPath = parentPath / d;
        if (!fs::exists(relPath))
            missingDeps.push_back(d);
    }

    if (!missingDeps.empty()) {
        std::string errorMessage = "Dependencies missing for " + gltfFile + ": ";
        for (size_t i = 0; i < missingDeps.size(); i++) {
            if (i > 0)
                errorMessage += ", ";
            errorMessage += missingDeps[i];
        }
        throw BuildDepMissingException(errorMessage, missingDeps);
    }
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
        // Validate GLTF dependencies before conversion
        validateGltfDependencies(inputObj, inputPath.parent_path());

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

    // Validate dependencies (for OBJ files or converted GLTF)
    validateDependencies(actualInputObj, inputPath.parent_path());

    // Build the nexus file
    performNexusBuild(actualInputObj, outFile);

    return outFile;
}

std::optional<ModelGeoref> detectModelGeoref(const std::string& inputObj) {
    const fs::path objPath(inputObj);
    const fs::path dir = objPath.parent_path();
    const fs::path parent = dir.parent_path();
    const std::string stem = objPath.stem().string();

    // Search order: explicit per-model sidecar, generic dataset sidecar, then the
    // OpenDroneMap reference_lla.json (co-located, one level up, or in an opensfm/
    // sibling for full ODM project layouts).
    const std::vector<fs::path> candidates = {
        dir / (stem + ".geo.json"),
        dir / "georef.json",
        dir / "reference_lla.json",
        parent / "reference_lla.json",
        parent / "opensfm" / "reference_lla.json",
    };

    // Returns the first present numeric value among keys, or def when none match.
    const auto getNum = [](const json& j, std::initializer_list<const char*> keys,
                           std::optional<double> def) -> std::optional<double> {
        for (const char* k : keys) {
            if (j.contains(k) && j[k].is_number())
                return j[k].get<double>();
        }
        return def;
    };

    for (const auto& c : candidates) {
        std::error_code ec;
        if (!fs::exists(c, ec) || ec)
            continue;

        try {
            std::ifstream in(c.string());
            if (!in.good())
                continue;

            json j;
            in >> j;

            const auto lat = getNum(j, {"latitude", "lat"}, std::nullopt);
            const auto lon = getNum(j, {"longitude", "lon", "lng"}, std::nullopt);
            const auto alt = getNum(j, {"altitude", "alt", "elevation"}, 0.0);

            if (!lat.has_value() || !lon.has_value())
                continue;

            if (*lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0) {
                LOGD << "Ignoring out-of-range georeference in " << c.string() << " (lat=" << *lat
                     << ", lon=" << *lon << ")";
                continue;
            }

            LOGD << "Model georeference found in " << c.string() << ": lat=" << *lat
                 << " lon=" << *lon << " alt=" << *alt;
            return ModelGeoref{*lat, *lon, alt.value_or(0.0)};
        } catch (const std::exception& e) {
            LOGD << "Could not parse georeference sidecar " << c.string() << ": " << e.what();
        }
    }

    return std::nullopt;
}

std::string buildModel3DTiles(const std::string& inputObj, const std::string& outputDir,
                              bool overwrite, std::optional<ModelGeoref> georef,
                              bool autoDetectGeoref) {
    TempDirGuard tempGuard;
    fs::path inputPath(inputObj);
    std::string actualInputObj = inputObj;

    // Resolve georeferencing from sidecars next to the ORIGINAL input (before any
    // GLTF->OBJ conversion moves us into a temp directory). An explicit georef
    // argument always wins.
    if (!georef.has_value() && autoDetectGeoref)
        georef = detectModelGeoref(inputObj);

    // Convert GLTF/GLB to OBJ if needed (same pre-processing as buildNexus)
    if (isGltfFile(inputPath)) {
        validateGltfDependencies(inputObj, inputPath.parent_path());
        actualInputObj = convertGltfToObj(inputObj, tempGuard);
        inputPath = fs::path(actualInputObj);
    }

    // Validate dependencies (for OBJ files or converted GLTF)
    validateDependencies(actualInputObj, inputPath.parent_path());

    const fs::path outDir(outputDir);

    // Handle an existing output directory
    if (fs::exists(outDir) && !fs::is_empty(outDir)) {
        if (overwrite)
            io::assureIsRemoved(outDir);
        else
            throw AppException("Directory " + outDir.string() +
                               " already exists (delete it first)");
    }

    // Fail fast with a typed exception if the Obj2Tiles binary is unavailable, so
    // callers (e.g. the build pipeline) can treat 3D Tiles as a best-effort,
    // non-blocking artifact and keep producing Nexus.
    if (obj2tiles::findObj2TilesBinary().empty())
        throw Obj2TilesException(
            "Obj2Tiles binary not found; cannot generate 3D Tiles. Set DDB_OBJ2TILES_PATH "
            "or place Obj2Tiles next to the DroneDB executable.");

    // Produce into a sibling temporary directory, then atomically move it into place
    // so a crashed/partial run never leaves an incomplete tileset at outDir.
    fs::path parent = outDir.parent_path();
    if (parent.empty())
        parent = fs::current_path();
    io::assureFolderExists(parent);

    const fs::path tempOut =
        parent / (outDir.filename().string() + "-temp-" + utils::generateRandomString(16));
    io::assureIsRemoved(tempOut);
    io::assureFolderExists(tempOut);

    obj2tiles::Obj2TilesOptions opts;
    if (georef.has_value()) {
        // Georeferenced tiling: Obj2Tiles builds the ECEF transform from the origin,
        // placing the model's local frame at the given WGS84 coordinates.
        opts.localMode = false;
        opts.lat = georef->latitude;
        opts.lon = georef->longitude;
        opts.alt = georef->altitude;
        LOGD << "Building georeferenced 3D Tiles for " << inputObj << " at lat=" << georef->latitude
             << " lon=" << georef->longitude << " alt=" << georef->altitude;
    } else {
        // Non-georeferenced (local) tiling: identity transform, parity with the
        // current non-georeferenced Nexus viewer.
        opts.localMode = true;
        LOGD << "Building non-georeferenced (local) 3D Tiles for " << inputObj;
    }

    std::string err;
    const bool ok = obj2tiles::runObj2Tiles(actualInputObj, tempOut, opts, err);
    if (!ok) {
        io::assureIsRemoved(tempOut);
        throw Obj2TilesException("Could not build 3D Tiles for " + inputObj + ": " + err);
    }

    // Atomic swap into place.
    if (fs::exists(outDir))
        io::assureIsRemoved(outDir);
    io::rename(tempOut, outDir);

    return (outDir / "tileset.json").string();
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
        if (mtlFile.size() >= 2 && mtlFile[0] == '"' && mtlFile[mtlFile.size() - 1] == '"')
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

std::vector<std::string> getGltfDependencies(const std::string& gltf) {
    if (!fs::exists(gltf))
        throw FSException(gltf + " does not exist");

    fs::path p(gltf);
    fs::path parentPath = p.parent_path();

    // Determine if GLTF (text JSON) or GLB (binary)
    std::string ext = p.extension().string();
    utils::toLower(ext);

    json gltfJson;

    try {
        if (ext == ".gltf") {
            // Read as text JSON file
            std::ifstream file(gltf);
            if (!file.is_open())
                throw FSException("Cannot open GLTF file: " + gltf);

            try {
                file >> gltfJson;
            } catch (const json::exception& e) {
                throw AppException("Invalid GLTF file: JSON parse error: " + std::string(e.what()));
            }
        } else if (ext == ".glb") {
            // Read binary GLB and extract JSON chunk
            gltfJson = readGlbJson(gltf);
        } else {
            throw AppException("File is not a GLTF or GLB: " + gltf);
        }

        // Validate basic GLTF structure
        if (!gltfJson.contains("asset")) {
            throw AppException("Invalid GLTF file: missing 'asset' property");
        }

        // Parse and extract dependencies
        return parseGltfJson(gltfJson, parentPath);

    } catch (const AppException&) {
        throw;  // Re-throw our exceptions
    } catch (const std::exception& e) {
        throw AppException("Error reading GLTF file: " + std::string(e.what()));
    }
}

}  // namespace ddb
