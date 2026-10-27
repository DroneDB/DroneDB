/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "3d.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include "bm3d_runner.h"
#include "build_layout.h"
#include "buildlock.h"
#include "deppath.h"
#include "exceptions.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "obj2tiles_runner.h"
#include "plyconv.h"
#include "randomutil.h"
#include "stringutil.h"

namespace ddb {
namespace {
// Check if file extension is GLTF or GLB
bool isGltfFile(const fs::path& filePath) {
    return io::Path(filePath).checkExtension({"gltf", "glb"});
}

// Check if file extension is GLB (binary format with embedded resources)
bool isGlbFile(const fs::path& filePath) {
    return io::Path(filePath).checkExtension({"glb"});
}

// Check if file extension is STL
bool isStlFile(const fs::path& filePath) {
    return io::Path(filePath).checkExtension({"stl"});
}

// Check if file extension is PLY
bool isPlyFile(const fs::path& filePath) {
    return io::Path(filePath).checkExtension({"ply"});
}

// Helper to check if a URI is a data URI (embedded base64 content)
bool isDataUri(const std::string& uri) {
    return uri.starts_with("data:");
}

// Helper to validate and sanitize a relative path (prevent path traversal attacks)
bool isSafePath(const std::string& path, const fs::path& parentPath) {
    try {
        // Resolve the path relative to parent
        fs::path resolvedPath = (parentPath / io::fromUtf8(path)).lexically_normal();
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

// Read and parse GLB file to extract JSON chunk
json readGlbJson(const std::string& glbPath) {
    std::ifstream file(io::fromUtf8(glbPath), std::ios::binary);
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

    // chunk.chunkLength drives the allocation below, so it is what has to fit in the file:
    // a corrupt or hostile GLB could otherwise ask for up to 4 GiB.
    constexpr uint64_t GlbHeaderSize = 12;
    constexpr uint64_t GlbChunkHeaderSize = 8;

    std::error_code sizeErr;
    const auto fileSize = fs::file_size(io::fromUtf8(glbPath), sizeErr);
    if (sizeErr)
        throw FSException("Cannot determine size of GLB file: " + glbPath);

    if (GlbHeaderSize + GlbChunkHeaderSize + static_cast<uint64_t>(chunk.chunkLength) > fileSize)
        throw AppException("Invalid GLB file: JSON chunk length exceeds file size");

    // header.length is advisory for our purposes: some exporters overstate it while the
    // JSON chunk is intact, so the file stays usable and we only leave a trace.
    if (header.length > fileSize)
        LOGW << "GLB " << glbPath << " declares a length of " << header.length
             << " bytes but the file is " << fileSize;

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

// Reads the JSON document of a .gltf (text) or .glb (JSON chunk) and checks the
// basic glTF structure.
// @throws FSException if the file does not exist or cannot be opened.
// @throws AppException if it is not a valid glTF/GLB.
json readGltfDocument(const std::string& gltf) {
    if (!fs::exists(io::fromUtf8(gltf)))
        throw FSException(gltf + " does not exist");

    // Determine if GLTF (text JSON) or GLB (binary)
    const std::string ext = io::lowerExtension(io::fromUtf8(gltf));

    json gltfJson;

    try {
        if (ext == "gltf") {
            // Read as text JSON file
            std::ifstream file(io::fromUtf8(gltf));
            if (!file.is_open())
                throw FSException("Cannot open GLTF file: " + gltf);

            try {
                file >> gltfJson;
            } catch (const json::exception& e) {
                throw AppException("Invalid GLTF file: JSON parse error: " + std::string(e.what()));
            }
        } else if (ext == "glb") {
            // Read binary GLB and extract JSON chunk
            gltfJson = readGlbJson(gltf);
        } else {
            throw AppException("File is not a GLTF or GLB: " + gltf);
        }

        // Validate basic GLTF structure
        if (!gltfJson.contains("asset")) {
            throw AppException("Invalid GLTF file: missing 'asset' property");
        }
    } catch (const AppException&) {
        throw;  // Re-throw our exceptions
    } catch (const std::exception& e) {
        throw AppException("Error reading GLTF file: " + std::string(e.what()));
    }

    return gltfJson;
}

bool isHttpUri(const std::string& uri) {
    return str::startsWithCI(uri, "http://") || str::startsWithCI(uri, "https://");
}

// Validate the file dependencies (buffer and image URIs) of a glTF or GLB.
//  - data: URIs are embedded content: nothing to check;
//  - http(s) URIs are never fetched and are not dependencies: they are left to
//    the converter, whose failure (or an unresolvable texture in its output) is
//    a plain AppException, never a pending build (see prepareModelInput);
//  - absolute paths and file: URIs, and relative URIs that leave the glTF's
//    folder (e.g. "../shared/tex.png"), can never be satisfied: only files in the
//    glTF's folder or below are staged for the converted model, so they fail
//    with AppException naming the URI instead of leaving the build pending;
//  - other relative URIs must exist: missing ones are reported (normalised,
//    relative to the glTF's folder) with BuildDepMissingException.
void validateGltfDependencies(const std::string& gltfFile, const fs::path& parentPath) {
    const json gltfJson = readGltfDocument(gltfFile);
    const fs::path baseDir = parentPath.empty() ? fs::path(".") : parentPath;
    std::vector<std::string> missingDeps;

    const auto check = [&](const char* kind, const std::string& uri) {
        if (isDataUri(uri))
            return;
        if (isHttpUri(uri)) {
            LOGD << gltfFile << " references the remote " << kind << " " << uri
                 << ", which is not fetched";
            return;
        }
        if (deppath::isAbsolute(uri))
            throw AppException(gltfFile + " references the " + kind + " '" + uri +
                               "' by an absolute path or URI, which can never be satisfied");

        const std::string name = deppath::normalize(uri);
        if (name.empty() || name == ".")
            return;
        if (deppath::isAbsolute(name) || !deppath::isInside(baseDir / io::fromUtf8(name), baseDir))
            throw AppException(gltfFile + " references the " + kind + " '" + uri +
                               "' outside its folder, which can never be satisfied (only files in "
                               "the glTF's folder or its subfolders are used)");

        std::error_code ec;
        if (!fs::exists(baseDir / io::fromUtf8(name), ec) || ec) {
            if (std::find(missingDeps.begin(), missingDeps.end(), name) == missingDeps.end())
                missingDeps.push_back(name);
        }
    };

    try {
        for (const char* section : {"buffers", "images"}) {
            if (!gltfJson.contains(section) || !gltfJson[section].is_array())
                continue;
            const char* kind = std::string(section) == "buffers" ? "buffer" : "image";
            for (const auto& item : gltfJson[section]) {
                if (item.is_object() && item.contains("uri") && item["uri"].is_string())
                    check(kind, item["uri"].get<std::string>());
            }
        }
    } catch (const json::exception& e) {
        throw AppException("Error parsing GLTF JSON: " + std::string(e.what()));
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

// One file dependency of an OBJ. Satisfied when any candidate exists.
struct ObjDependency {
    // Reported name: normalised, relative to the OBJ's folder ("m.mtl",
    // "tex.png", "mats/tex.png" for a texture of "mats/m.mtl").
    std::string name;
    // Paths to probe, in lookup order.
    std::vector<fs::path> candidates;
};

// Returns the first whitespace-delimited token of a line and the rest (trimmed).
std::string splitKeyword(const std::string& line, std::string& rest) {
    const auto begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        rest.clear();
        return "";
    }
    const auto end = line.find_first_of(" \t\r", begin);
    const std::string keyword =
        line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    rest = end == std::string::npos ? "" : line.substr(end);
    str::trim(rest);
    return keyword;
}

// Extracts the file name from the arguments of an MTL texture statement
// ("map_Kd [options] file"). A double-quoted name is taken verbatim (spaces
// allowed); otherwise leading options ("-bm 1.0", "-o 0 0 0", "-clamp on",
// "-type sphere", ...) are skipped and the rest of the line is the name. Unlike a
// word-boundary regex this keeps a leading "/", "./" or "../", which the path
// contract needs to see.
std::optional<std::string> parseTextureArgument(const std::string& args) {
    const auto q1 = args.find('"');
    if (q1 != std::string::npos) {
        const auto q2 = args.find('"', q1 + 1);
        if (q2 != std::string::npos && q2 > q1 + 1)
            return args.substr(q1 + 1, q2 - q1 - 1);
    }

    std::vector<std::string> tokens;
    {
        std::istringstream iss(args);
        std::string t;
        while (iss >> t)
            tokens.push_back(t);
    }

    const auto isNumber = [](const std::string& t) {
        char* end = nullptr;
        std::strtod(t.c_str(), &end);
        return end != t.c_str() && *end == '\0';
    };

    size_t i = 0;
    while (i < tokens.size() && tokens[i].size() > 1 && tokens[i][0] == '-' &&
           str::isAlphaChar(tokens[i][1])) {
        const std::string opt = tokens[i++];
        if (opt == "-type" || opt == "-imfchan") {
            if (i < tokens.size())
                ++i;  // one word argument
            continue;
        }
        while (i < tokens.size() &&
               (isNumber(tokens[i]) || tokens[i] == "on" || tokens[i] == "off"))
            ++i;
    }

    if (i >= tokens.size())
        return std::nullopt;

    std::string name = tokens[i];
    for (size_t k = i + 1; k < tokens.size(); ++k)
        name += " " + tokens[k];
    return name;
}

[[noreturn]] void throwUnsatisfiable(const std::string& sourceFile, const std::string& ref,
                                     const char* why) {
    throw AppException("Unsatisfiable dependency in " + sourceFile + ": '" + ref + "' " + why +
                       " and can never be provided by adding files to the dataset");
}

// Path contract for OBJ dependencies (see deppath.h):
//   - mtllib resolves against the OBJ's folder;
//   - an MTL texture is present if it exists relative to the MTL's folder,
//     otherwise relative to the OBJ's folder; it is reported as
//     "<MTL folder relative to the OBJ folder>/<texture>";
//   - absolute references, and references escaping datasetRoot (when known),
//     throw AppException.
std::vector<ObjDependency> collectObjDependencies(const std::string& obj,
                                                  const fs::path& datasetRoot) {
    if (!fs::exists(io::fromUtf8(obj)))
        throw FSException(obj + " does not exist");

    const fs::path objDir = io::fromUtf8(obj).parent_path();
    const bool checkRoot = !datasetRoot.empty();

    std::vector<ObjDependency> deps;
    const auto add = [&deps](ObjDependency d) {
        for (const auto& existing : deps)
            if (existing.name == d.name)
                return;
        deps.push_back(std::move(d));
    };

    static const std::vector<std::string> textureKeys = {"map_Ka", "map_Kd", "map_Ks", "map_Ns",
                                                         "map_d", "disp", "decal", "bump",
                                                         "map_bump", "refl", "map_Pr", "map_Pm",
                                                         "map_Ps", "map_Ke"};

    std::ifstream fin(io::fromUtf8(obj));
    std::string line;
    while (std::getline(fin, line)) {
        std::string mtlRef;
        if (splitKeyword(line, mtlRef) != "mtllib" || mtlRef.empty())
            continue;

        // Remove double quotes if present
        if (mtlRef.size() >= 2 && mtlRef.front() == '"' && mtlRef.back() == '"')
            mtlRef = mtlRef.substr(1, mtlRef.size() - 2);

        const std::string mtlName = deppath::normalize(mtlRef);
        if (mtlName.empty() || mtlName == ".")
            continue;
        if (deppath::isAbsolute(mtlName))
            throwUnsatisfiable(obj, mtlRef, "is an absolute path");

        const fs::path mtlPath = objDir / io::fromUtf8(mtlName);
        if (checkRoot && !deppath::isInside(mtlPath, datasetRoot))
            throwUnsatisfiable(obj, mtlRef, "points outside the dataset");

        add({mtlName, {mtlPath}});

        if (!fs::exists(mtlPath))
            continue;

        const fs::path mtlRelDir = io::fromUtf8(mtlName).parent_path();
        const fs::path mtlDir = mtlPath.parent_path();

        // Parse MTL
        std::string mtlLine;
        std::ifstream mtlFin(mtlPath);
        while (std::getline(mtlFin, mtlLine)) {
            std::string rest;
            const std::string key = splitKeyword(mtlLine, rest);
            if (std::find(textureKeys.begin(), textureKeys.end(), key) == textureKeys.end())
                continue;

            const auto textureRef = parseTextureArgument(rest);
            if (!textureRef.has_value())
                continue;

            const std::string tex = deppath::normalize(textureRef.value());
            if (tex.empty() || tex == ".")
                continue;
            if (deppath::isAbsolute(tex))
                throwUnsatisfiable(io::toUtf8(mtlPath), textureRef.value(), "is an absolute path");

            ObjDependency d;
            d.name = mtlRelDir.empty()
                         ? tex
                         : deppath::normalize(io::toGenericUtf8(mtlRelDir / io::fromUtf8(tex)));
            for (const auto& c : {mtlDir / io::fromUtf8(tex), objDir / io::fromUtf8(tex)}) {
                if (checkRoot && !deppath::isInside(c, datasetRoot))
                    continue;
                if (std::find(d.candidates.begin(), d.candidates.end(), c) == d.candidates.end())
                    d.candidates.push_back(c);
            }
            if (d.candidates.empty())
                throwUnsatisfiable(
                    io::toUtf8(mtlPath), textureRef.value(), "points outside the dataset");

            add(std::move(d));
        }
    }

    return deps;
}

// Validate that all file dependencies of an OBJ exist. Missing names are
// reported relative to the OBJ's folder (BuildDepMissingException); see
// collectObjDependencies for the path contract. datasetRoot is empty for
// files produced by a converter in a temp folder.
void validateDependencies(const std::string& objFile, const fs::path& datasetRoot) {
    const auto deps = collectObjDependencies(objFile, datasetRoot);
    std::vector<std::string> missingDeps;

    for (const auto& d : deps) {
        bool found = false;
        for (const auto& c : d.candidates) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) {
                found = true;
                break;
            }
        }
        if (!found)
            missingDeps.push_back(d.name);
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

// The OBJ converted from a glTF lives in a temp folder, but its MTL keeps the
// glTF's relative image URIs (embedded images are extracted next to it by the
// converter). Stage external images there too, so that dependency validation,
// bm3d and Obj2Tiles find them. Only images inside the glTF's folder are
// staged, matching validateGltfDependencies. Images are copied, never linked, so
// nothing writing into the temp folder can alter the dataset's files.
void stageGltfTextures(const fs::path& gltfDir, const std::string& convertedObj) {
    const fs::path tempDir = io::fromUtf8(convertedObj).parent_path();

    for (const auto& d : collectObjDependencies(convertedObj, fs::path())) {
        bool present = false;
        for (const auto& c : d.candidates) {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) {
                present = true;
                break;
            }
        }
        if (present || !isSafePath(d.name, gltfDir))
            continue;

        const fs::path src = gltfDir / io::fromUtf8(d.name);
        std::error_code ec;
        if (!fs::is_regular_file(src, ec) || ec)
            continue;

        const fs::path dst = tempDir / io::fromUtf8(d.name);
        if (!deppath::isInside(dst, tempDir))
            continue;

        fs::create_directories(dst.parent_path(), ec);
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec)
            LOGW << "Cannot stage texture " << io::toUtf8(src) << " for the converted model: "
                 << ec.message();
        else
            LOGD << "Staged texture " << io::toUtf8(src) << " -> " << io::toUtf8(dst);
    }
}

// The files a conversion with Assimp wrote in the temp folder
struct AssimpConversion {
    std::string geomPath;  // OBJ or PLY
    std::string mtlPath;   // MTL next to the OBJ, empty for a PLY or when there is none
};

// Convert a model Assimp reads (glTF/GLB, STL) to OBJ in a temporary directory. With
// preferPLYIfNoUV a model without texture coordinates is written as PLY instead (keeps
// vertex colours as properties of the mesh); Obj2Tiles reads OBJ only, and
// the OBJ exporter writes vertex colours as `v x y z r g b`.
AssimpConversion convertWithAssimp(const std::string& input,
                                   io::ScopedTempDir& tempGuard,
                                   bool preferPLYIfNoUV) {
    LOGD << "Converting " << input << " to OBJ first";

    fs::path inputPath = io::fromUtf8(input);
    fs::path tempDir = fs::temp_directory_path() / ("ddb_assimp_convert_" + randomString(16));
    fs::create_directories(tempDir);
    tempGuard.reset(tempDir);

    AssimpConversion converted;
    std::string outputBasePath = io::toUtf8(tempDir / inputPath.stem());

    convertGltfTo3dModel(input,
                         outputBasePath,
                         converted.geomPath,
                         converted.mtlPath,
                         false,
                         preferPLYIfNoUV);

    LOGD << "Converted to: " << converted.geomPath;
    return converted;
}

// glTF/GLB to OBJ (or PLY, see convertWithAssimp), with the images the OBJ's MTL
// references staged next to it.
std::string convertGltfToObj(const std::string& inputGltf,
                             io::ScopedTempDir& tempGuard,
                             bool preferPLYIfNoUV) {
    const AssimpConversion converted = convertWithAssimp(inputGltf, tempGuard, preferPLYIfNoUV);

    if (!converted.mtlPath.empty())
        stageGltfTextures(io::fromUtf8(inputGltf).parent_path(), converted.geomPath);

    return converted.geomPath;
}

// Consumer of a prepared model (see prepareModelInput).
enum class ModelConsumer { Bm3d, Tiles };

// A model ready for bm3d or Obj2Tiles, plus the stats a converter computed.
struct PreparedModel {
    std::string path;               // file to hand to the consumer
    std::optional<ModelInfo> info;  // face count + bounds when a converter supplied them
};

// Validates a model written by a converter into a temp folder. Every
// satisfiable dependency of the original input was checked and staged before the
// conversion, so a name still missing here can never be satisfied by uploading a
// file: its temp-folder-relative name must not reach ".pending" (it would retry
// forever). It is reported as a plain AppException naming the original input.
void validateConvertedDependencies(const std::string& convertedObj, const std::string& originalInput) {
    try {
        validateDependencies(convertedObj, fs::path());
    } catch (const BuildDepMissingException& e) {
        std::string names;
        for (const auto& d : e.getMissingDependencies())
            names += (names.empty() ? "" : ", ") + d;
        throw AppException("The model converted from " + originalInput +
                           " references files that cannot be resolved: " + names);
    }
}

// Single owner of the input-format dispatch for buildBm3d and buildModel3DTiles.
// Validates the dependencies of the input and converts it when the consumer
// cannot read it as is:
//   - glTF/GLB: validated, then converted to OBJ in a temp folder. For bm3d a
//     glTF without UVs becomes a PLY; for Obj2Tiles it is always an OBJ.
//   - STL: bm3d reads it natively; Obj2Tiles reads OBJ only, so it becomes an OBJ in a temp
//     folder (an STL has no texture: the OBJ has no dependency).
//   - PLY: never text-scanned as an OBJ. Its TextureFile (if any) is checked from
//     the header against the dataset root. bm3d reads an untextured PLY natively;
//     otherwise the streaming converter writes OBJ (+ MTL + texture) in a temp
//     folder and reports face count and bounds.
//   - anything else (OBJ): validated in place.
// The dataset root applies to the original input only; converted outputs in a
// temp folder are validated without it. tempGuard owns that temp folder.
PreparedModel prepareModelInput(const std::string& input, ModelConsumer consumer,
                                const std::string& datasetRoot, io::ScopedTempDir& tempGuard) {
    const fs::path inputPath = io::fromUtf8(input);

    if (isGltfFile(inputPath)) {
        validateGltfDependencies(input, inputPath.parent_path());
        PreparedModel m;
        m.path = convertGltfToObj(input, tempGuard, consumer == ModelConsumer::Bm3d);
        // An Assimp-exported PLY has no file dependencies.
        if (!isPlyFile(io::fromUtf8(m.path)))
            validateConvertedDependencies(m.path, input);
        return m;
    }

    if (isStlFile(inputPath)) {
        if (consumer == ModelConsumer::Bm3d) {
            LOGD << "STL, handing it to bm3d as is: " << input;
            return {input, std::nullopt};
        }

        PreparedModel m;
        m.path = convertWithAssimp(input, tempGuard, false).geomPath;
        validateConvertedDependencies(m.path, input);
        return m;
    }

    if (isPlyFile(inputPath)) {
        // Header-only: throws BuildDepMissingException for a missing texture and
        // AppException for an absolute or root-escaping TextureFile.
        const std::string texture = getPlyTexture(input, datasetRoot);

        if (consumer == ModelConsumer::Bm3d && texture.empty()) {
            LOGD << "Untextured PLY, handing it to bm3d as is: " << input;
            return {input, std::nullopt};
        }

        const fs::path tempDir =
            fs::temp_directory_path() / ("ddb_ply_convert_" + randomString(16));
        fs::create_directories(tempDir);
        tempGuard.reset(tempDir);

        PlyToObjOptions opts;
        opts.withTexture = true;
        opts.datasetRoot = datasetRoot;
        const PlyToObjResult res =
            convertPlyToObj(input, io::toUtf8(tempDir / "model"), opts);
        LOGD << "PLY converted to " << res.objPath << " (" << res.info.faceCount << " faces"
             << (res.mtlPath.empty() ? "" : ", textured") << ")";

        validateConvertedDependencies(res.objPath, input);

        PreparedModel m;
        m.path = res.objPath;
        if (res.info.hasBounds)
            m.info = res.info;
        return m;
    }

    validateDependencies(input, io::fromUtf8(datasetRoot));
    return {input, std::nullopt};
}

// "out/3dtiles/" and "out/3dtiles//" -> "out/3dtiles", so the lock, the temp and
// the -old siblings are derived from the folder name.
fs::path stripTrailingSeparators(const std::string& dir) {
    fs::path p = io::fromUtf8(dir);
    while (!p.empty() && !p.has_filename() && p.has_relative_path())
        p = p.parent_path();
    return p;
}

// Use the expected sentinel, not directory existence: the final directory can
// itself be a partial output. Throwing filesystem calls keep inspection errors
// from being mistaken for permission to delete the backup.
bool tilesSentinelComplete(const fs::path& dir) {
    const fs::path sentinel = dir / TilesetFileName;
    return fs::is_regular_file(sentinel) && fs::file_size(sentinel) > 0;
}

// Caller holds the tiles lock; never recursively acquire it here.
void recoverTilesSwap(const fs::path& outDir, const fs::path& oldDir) {
    if (!fs::exists(oldDir))
        return;

    if (tilesSentinelComplete(outDir)) {
        LOGD << "Removing superseded 3D Tiles " << io::toUtf8(oldDir);
        io::assureIsRemoved(oldDir);
    } else if (tilesSentinelComplete(oldDir)) {
        // Remove the incomplete final first: if either operation fails, the
        // last usable tileset still resides in oldDir.
        io::assureIsRemoved(outDir);
        LOGI << "Restoring previous 3D Tiles from " << io::toUtf8(oldDir);
        io::rename(oldDir, outDir);
    } else {
        LOGW << "Keeping incomplete 3D Tiles backup " << io::toUtf8(oldDir)
             << " until a complete replacement is published";
    }
}

// Perform the actual bm3d build: bm3d is a program of its own, so that none of its code (VCGLib,
// corto) is loaded into this process
void performBm3dBuild(const std::string& inputFile,
                      const std::string& outputFile,
                      const BuildOptions& options) {
    bm3d::runBm3dBuild(io::fromUtf8(inputFile),
                       io::fromUtf8(outputFile),
                       {},
                       process::systemProcessRunner(),
                       options);
}
}  // namespace

bool isModel3DTilesComplete(const fs::path& outputDir) {
    try {
        return tilesSentinelComplete(outputDir);
    } catch (const fs::filesystem_error& e) {
        LOGW << "Cannot inspect 3D Tiles sentinel in " << io::toUtf8(outputDir) << ": " << e.what();
        return false;
    }
}

bool hasRecoverableModel3DTilesSwap(const fs::path& outputDir) {
    const fs::path oldDir = backupFolderPath(outputDir);
    return isModel3DTilesComplete(oldDir) && !isModel3DTilesComplete(outputDir);
}

void recoverModel3DTilesSwap(const std::string& outputDir) {
    const fs::path outDir = stripTrailingSeparators(outputDir);
    BuildLock tilesLock(io::toUtf8(outDir));
    const fs::path oldDir = backupFolderPath(outDir);
    recoverTilesSwap(outDir, oldDir);
}

std::string buildBm3d(const std::string& inputObj,
                      const std::string& outputBm3d,
                      bool overwrite,
                      const std::string& datasetRoot,
                      const BuildOptions& options) {
    // Default output: next to the original input (never inside a conversion temp folder).
    const auto outFile = outputBm3d.empty()
                             ? io::toUtf8(io::fromUtf8(inputObj).replace_extension(".bm3d"))
                             : outputBm3d;

    // Check if output file already exists (overwrite deletes it, so it must not be the input)
    io::assureDistinctOutput(io::fromUtf8(inputObj), io::fromUtf8(outFile));
    if (fs::exists(io::fromUtf8(outFile))) {
        if (overwrite)
            io::assureIsRemoved(io::fromUtf8(outFile));
        else
            throw AppException("File " + outFile + " already exists (delete it first)");
    }

    // Validate dependencies and convert when needed (glTF/GLB, textured PLY).
    // Throws before bm3d runs, so a missing dependency never produces output.
    io::ScopedTempDir tempGuard;
    const PreparedModel model =
        prepareModelInput(inputObj, ModelConsumer::Bm3d, datasetRoot, tempGuard);

    // Build the bm3d file
    performBm3dBuild(model.path, outFile, options);

    return outFile;
}

std::optional<ModelGeoref> detectModelGeoref(const std::string& inputObj) {
    const fs::path objPath = io::fromUtf8(inputObj);
    const fs::path dir = objPath.parent_path();
    const fs::path parent = dir.parent_path();
    const std::string stem = io::toUtf8(objPath.stem());

    // Search order: explicit per-model sidecar, generic dataset sidecar, then the
    // OpenDroneMap reference_lla.json (co-located, one level up, or in an opensfm/
    // sibling for full ODM project layouts).
    const std::vector<fs::path> candidates = {
        dir / io::fromUtf8(stem + ".geo.json"),
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
            std::ifstream in(c);
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
                LOGD << "Ignoring out-of-range georeference in " << io::toUtf8(c) << " (lat=" << *lat
                     << ", lon=" << *lon << ")";
                continue;
            }

            LOGD << "Model georeference found in " << io::toUtf8(c) << ": lat=" << *lat
                 << " lon=" << *lon << " alt=" << *alt;
            return ModelGeoref{*lat, *lon, alt.value_or(0.0)};
        } catch (const std::exception& e) {
            LOGD << "Could not parse georeference sidecar " << io::toUtf8(c) << ": " << e.what();
        }
    }

    return std::nullopt;
}

namespace {

// The Obj2Tiles parameters for a model: the face-count heuristic picks the tile hierarchy, or
// DDB_OBJ2TILES_FORCE_DEFAULTS (--force-defaults) skips it; georeferenced when georef is given.
//
// Face count → (divisions, lods, octree) heuristic selects the right tile
// hierarchy depth for the model's complexity. Small models (<10K faces) get
// a single tile at the finest LOD; large models get progressively deeper
// splits, hard-capped at depth 6 (≤ 4096 finest-LOD tiles).
// --force-defaults / DDB_OBJ2TILES_FORCE_DEFAULTS env var skips the heuristic
// and uses hardcoded defaults for testing/debugging.
obj2tiles::Obj2TilesOptions chooseObj2TilesOptions(const std::string& inputObj, const PreparedModel& model,
                                                   const std::optional<ModelGeoref>& georef) {
    const std::string& actualInputObj = model.path;
    const bool forceDefaults = [] {
        const char* env = std::getenv("DDB_OBJ2TILES_FORCE_DEFAULTS");
        return env && env[0] != '0' && env[0] != '\0';
    }();

    ModelInfo info;
    obj2tiles::Obj2TilesOptions opts;
    if (forceDefaults) {
        // --force-defaults: skip heuristic, use hardcoded defaults (current behavior)
        opts.octree = true;
        opts.divisions = 3;
        opts.lods = 3;
        LOGD << "DDB_OBJ2TILES_FORCE_DEFAULTS set — skipping heuristic, "
             << "using defaults (divisions=3, lods=3, octree=true)";
    } else if (model.info.has_value()) {
        // The converter already counted faces and bounds: no Assimp re-import.
        info = *model.info;
        opts = computeObj2TilesOpts(info);
        LOGD << "Model " << inputObj << " has " << (info.faceCount / 1000)
             << "K faces (from the converter) → divisions=" << opts.divisions
             << ", lods=" << opts.lods << ", octree=" << (opts.octree ? "true" : "false");
    } else if (getModelInfo(actualInputObj, info)) {
        opts = computeObj2TilesOpts(info);
        LOGD << "Model " << actualInputObj << " has " << (info.faceCount / 1000)
             << "K faces → divisions=" << opts.divisions << ", lods=" << opts.lods
             << ", octree=" << (opts.octree ? "true" : "false");
    } else {
        // Fallback: preserve current hardcoded behavior when model stats unavailable.
        // Matches the "XXL" band (depth = 3 + 3 - 1 = 5, ≤ 1024 finest-LOD tiles).
        opts.octree = true;
        opts.divisions = 3;
        opts.lods = 3;
        LOGD << "Could not determine face count for " << actualInputObj
             << " — using fallback (divisions=3, lods=3, octree=true)";
    }
    // Preserve texture defaults (heuristic sets divisions/lods/octree only)
    opts.textureFormat = "Ktx2";
    opts.ktx2Quality = 192;
    opts.splitStrategy = "VertexMedian";
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
        // current non-georeferenced bm3d viewer.
        opts.localMode = true;
        LOGD << "Building non-georeferenced (local) 3D Tiles for " << inputObj;
    }
    return opts;
}

// Two-step swap: outDir -> "<outDir>-old", temp -> outDir, drop -old.
// A crash in between is repaired by recoverTilesSwap on the next run. outGuard owns tempOut and
// lets go of it once it is in place.
void publishTiles(const fs::path& tempOut, const fs::path& outDir, const fs::path& oldDir,
                  io::ScopedTempDir& outGuard) {
    std::error_code ec;
    // An incomplete backup may have been retained by recovery. Keep it through
    // publication rather than overwrite it with another incomplete final.
    const bool retainedBackup = fs::exists(oldDir);
    if (retainedBackup)
        io::assureIsRemoved(outDir);
    const bool hadPrevious = !retainedBackup && fs::exists(outDir, ec);
    if (hadPrevious) {
        try {
            io::rename(outDir, oldDir);
        } catch (const std::exception& e) {
            // Previous tiles are intact; the guard removes the new ones.
            throw AppException("Cannot move the previous 3D Tiles aside (" + std::string(e.what()) +
                               "); keeping them");
        }
    }

    try {
        io::rename(tempOut, outDir);
    } catch (const std::exception& e) {
        std::string restored;
        if (hadPrevious) {
            try {
                io::rename(oldDir, outDir);
                restored = "; previous tiles restored";
            } catch (const std::exception& re) {
                // recoverTilesSwap restores them on the next run.
                restored = "; previous tiles left in " + io::toUtf8(oldDir) + " (" + re.what() + ")";
            }
        }
        throw AppException("Cannot move the new 3D Tiles into place (" + std::string(e.what()) +
                           ")" + restored);
    }
    outGuard.release();

    if (hadPrevious || retainedBackup) {
        try {
            io::assureIsRemoved(oldDir);
        } catch (const std::exception& e) {
            // The new tileset is in place; the next run or cleanupBuild drops -old.
            LOGW << "Cannot remove previous 3D Tiles " << io::toUtf8(oldDir) << ": " << e.what();
        }
    }
}

}  // namespace

std::string buildModel3DTiles(const std::string& inputObj, const std::string& outputDir,
                              const Tiles3DOptions& options) {
    // Fail fast: if Obj2Tiles is unavailable, skip georef detection, conversion and
    // dependency validation entirely. Callers treat 3D Tiles as best-effort and keep
    // producing the bm3d file.
    if (obj2tiles::findObj2TilesBinary().empty())
        throw Obj2TilesException(
            "Obj2Tiles binary not found; cannot generate 3D Tiles. Set DDB_OBJ2TILES_PATH "
            "or place Obj2Tiles next to the DroneDB executable.");

    fs::path outDir = stripTrailingSeparators(outputDir);
    {
        const std::string name = io::toUtf8(outDir.filename());
        if (name.empty() || name == "." || name == "..")
            throw AppException("Invalid 3D Tiles output directory: " + outputDir);
    }
    if (!outDir.has_parent_path())
        outDir = fs::current_path() / outDir;
    const fs::path parent = outDir.parent_path();
    io::assureFolderExists(parent);

    const std::string outName = io::toUtf8(outDir.filename());
    const fs::path oldDir = backupFolderPath(outDir);

    // One tiles build per output folder ("<outDir>.building"), held until return.
    // It also gates removeOrphanTempFolders (cleanupBuild) for the -temp-/-old
    // siblings. Throws BuildInProgressException when another build holds it.
    BuildLock tilesLock(io::toUtf8(outDir));

    recoverTilesSwap(outDir, oldDir);

    if (!options.overwrite && fs::exists(outDir) && !fs::is_empty(outDir))
        throw AppException("Directory " + io::toUtf8(outDir) + " already exists (delete it first)");

    // Resolve georeferencing from sidecars next to the ORIGINAL input (before any
    // conversion moves us into a temp directory). An explicit georef argument
    // always wins.
    std::optional<ModelGeoref> georef = options.georef;
    if (!georef.has_value() && options.autoDetectGeoref)
        georef = detectModelGeoref(inputObj);

    // Validate dependencies and convert to OBJ when needed (glTF/GLB, PLY).
    io::ScopedTempDir inputGuard;
    const PreparedModel model =
        prepareModelInput(inputObj, ModelConsumer::Tiles, options.datasetRoot, inputGuard);

    // Produce into a sibling temporary directory; outDir is untouched until
    // Obj2Tiles succeeds. The guard removes the temp folder on any failure.
    const fs::path tempOut = parent / io::fromUtf8(outName + TempFolderInfix + randomString(16));
    io::ScopedTempDir outGuard;
    io::assureIsRemoved(tempOut);
    io::assureFolderExists(tempOut);
    outGuard.reset(tempOut);

    obj2tiles::Obj2TilesOptions opts = chooseObj2TilesOptions(inputObj, model, georef);
    opts.maxCores = options.build.threads;

    std::string err;
    if (!obj2tiles::runObj2Tiles(io::fromUtf8(model.path), tempOut, opts, err))
        throw Obj2TilesException("Could not build 3D Tiles for " + inputObj + ": " + err);

    publishTiles(tempOut, outDir, oldDir, outGuard);

    return io::toUtf8(outDir / TilesetFileName);
}

std::vector<std::string> getObjDependencies(const std::string& obj,
                                            const std::string& datasetRoot) {
    std::vector<std::string> deps;
    for (const auto& d : collectObjDependencies(obj, io::fromUtf8(datasetRoot)))
        deps.push_back(d.name);
    return deps;
}

EntryType identifyGltf(const fs::path& gltfFile) {
    // A text .gltf can embed all of its buffers as data URIs, and the DOM costs several
    // times the file size: past this point fall back to the extension alone.
    constexpr uintmax_t MaxClassifiableGltfSize = 64ull * 1024 * 1024;

    json gltfJson;

    try {
        if (isGlbFile(gltfFile)) {
            gltfJson = readGlbJson(io::toUtf8(gltfFile));
        } else {
            std::error_code sizeErr;
            const auto fileSize = fs::file_size(gltfFile, sizeErr);
            if (sizeErr)
                return EntryType::Generic;

            if (fileSize > MaxClassifiableGltfSize) {
                LOGD << io::toUtf8(gltfFile) << " is too large to classify (" << fileSize
                     << " bytes), assuming it is a model";
                return EntryType::Model;
            }

            std::ifstream file(gltfFile);
            if (!file.is_open())
                return EntryType::Generic;
            file >> gltfJson;
        }
    } catch (const std::exception& e) {
        LOGD << "Cannot classify " << io::toUtf8(gltfFile) << ", not a readable glTF: " << e.what();
        return EntryType::Generic;
    }

    // glTF 2.0 declares "meshes" as an array; glTF 1.0 declares it as an object
    // keyed by mesh id. Both wrap mesh entries shaped the same way otherwise.
    if (!gltfJson.contains("meshes") ||
        (!gltfJson["meshes"].is_array() && !gltfJson["meshes"].is_object())) {
        LOGD << io::toUtf8(gltfFile) << " declares no meshes, treating as generic";
        return EntryType::Generic;
    }

    for (const auto& mesh : gltfJson["meshes"]) {
        if (!mesh.contains("primitives") || !mesh["primitives"].is_array())
            continue;

        for (const auto& primitive : mesh["primitives"]) {
            if (!primitive.is_object())
                continue;

            // glTF 2.0: mode defaults to 4 (TRIANGLES) when omitted.
            // 4 = TRIANGLES, 5 = TRIANGLE_STRIP, 6 = TRIANGLE_FAN.
            int mode = 4;
            if (primitive.contains("mode") && primitive["mode"].is_number_integer())
                mode = primitive["mode"].get<int>();

            if (mode >= 4 && mode <= 6)
                return EntryType::Model;
        }
    }

    LOGD << io::toUtf8(gltfFile) << " has no triangle primitives, treating as generic";
    return EntryType::Generic;
}

/**
 * @brief Compute Obj2Tiles parameters based on model face count.
 *
 * Threshold bands:
 * - Tiny:    < 10K faces  → lods=1, divisions=0, octree=false  (depth=0,   ≤ 1 tile)
 * - Small:   10K–50K     → lods=2, divisions=0, octree=true   (depth=1,   ≤ 4 tiles)
 * - Medium:  50K–200K    → lods=2, divisions=1, octree=true   (depth=2,  ≤ 16 tiles)
 * - Large:   200K–750K   → lods=3, divisions=2, octree=true   (depth=4, ≤ 256 tiles)
 * - XL:      750K–3M     → lods=3, divisions=3, octree=true   (depth=5, ≤ 1024 tiles)
 * - XXL:     3M–12M      → lods=4, divisions=2, octree=true   (depth=5, ≤ 1024 tiles)
 * - Massive: > 12M       → lods=4, divisions=3, octree=true   (depth=6, ≤ 4096 tiles, hard cap)
 *
 * Note: Base-grid tiles (divisions) cull independently
 * per-tile, while LOD-hierarchy depth only adds refinement steps on top of the same
 * base grid. Favoring divisions over lods in the mid-to-large range yields finer
 * per-tile download granularity for the same total depth budget. Kept face-count as
 * the sole signal (not model area) because area is only meaningful in physical units
 * for georeferenced photogrammetry output; generic user-uploaded models (arbitrary
 * units, arbitrary orientation, orbit-viewed) would make an area-based divisions
 * calculation actively harmful.
 */
const int MAX_TILE_DEPTH = 6;

obj2tiles::Obj2TilesOptions computeObj2TilesOpts(const ModelInfo& info) {
    obj2tiles::Obj2TilesOptions opts;

    uint64_t faces = info.faceCount;
    bool octree = true;
    int lods = 3;
    int divisions = 2;

    if (faces < 10000) {
        // Tiny: < 10K faces
        lods = 1;
        divisions = 0;
        octree = false;
    } else if (faces < 50000) {
        // Small: 10K–50K
        lods = 2;
        divisions = 0;
    } else if (faces < 200000) {
        // Medium: 50K–200K
        lods = 2;
        divisions = 1;
    } else if (faces < 750000) {
        // Large: 200K–750K
        lods = 3;
        divisions = 2;
    } else if (faces < 3000000) {
        // XL: 750K–3M
        lods = 3;
        divisions = 3;
    } else if (faces < 12000000) {
        // XXL: 3M–12M
        lods = 4;
        divisions = 2;
    } else {
        // Massive: > 12M (hard cap at depth 6)
        lods = 4;
        divisions = 3;
    }

    // Clamp: ensure depth never exceeds MAX_TILE_DEPTH
    int depth = octree ? (lods + divisions - 1) : divisions;
    if (octree && depth > MAX_TILE_DEPTH) {
        // Reduce lods first (preserves finer base grid, coarser LOD hierarchy)
        lods = MAX_TILE_DEPTH - divisions + 1;
        if (lods < 1) lods = 1;
    }

    opts.lods = lods;
    opts.divisions = divisions;
    opts.octree = octree;

    // Preserve texture defaults (same as current hardcoded values)
    opts.textureFormat = "Ktx2";
    opts.ktx2Quality = 192;
    opts.splitStrategy = "VertexMedian";

    // KTX2 encoding dominates Obj2Tiles run time and runs one tile per core.
    // With at most 16 finest-LOD tiles (depth <= 2) most cores sit idle while
    // each tile encodes its textures single-threaded, so the encoder gets
    // threads of its own. Measured with Obj2Tiles 1.8.0: 101K faces / 42 4K
    // textures 42 s -> 26 s; 8 threads gave no further gain. Deeper hierarchies
    // (256+ finest tiles) already saturate the cores: no speed-up there, only
    // +1-2 GB peak memory, so they keep the single-threaded encoder.
    const int finalDepth = octree ? (lods + divisions - 1) : divisions;
    opts.ktx2Threads = finalDepth <= 2 ? 4 : 0;

    LOGD << "computeObj2TilesOpts: faces=" << faces
         << " → lods=" << lods << ", divisions=" << divisions
         << ", octree=" << (octree ? "true" : "false")
         << ", depth=" << finalDepth << ", ktx2Threads=" << opts.ktx2Threads;

    return opts;
}

}  // namespace ddb
