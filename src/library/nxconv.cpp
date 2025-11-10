// nxconv.cpp — correct version for libktx 4.x

#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "3d.h"
#include "logger.h"
#include "exceptions.h"
#include "utils.h"

// libktx + stb_image_write for KTX2->PNG conversion
#include <ktx.h>

#ifndef NXCONV_STB_IMAGE_WRITE_IMPLEMENTATION
#define NXCONV_STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#ifdef NXCONV_STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace ddb {

/// Check if the scene contains UV texture coordinates
static bool sceneHasUVs(const aiScene* s) {
    if (!s)
        return false;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        if (s->mMeshes[m]->mTextureCoords[0])
            return true;
    }
    return false;
}

/// Check if the scene contains vertex color data
static bool sceneHasVertexColors(const aiScene* s) {
    if (!s)
        return false;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        if (s->mMeshes[m]->mColors[0])
            return true;
    }
    return false;
}


/// Convert a KTX2 texture file to PNG format
/// @param ktxPath Path to the input KTX2 file
/// @param pngPath Path to the output PNG file
/// @throws AppException if conversion fails
static void convertKtx2ToPng(const fs::path& ktxPath, const fs::path& pngPath) {
    LOGD << "[ktx2->png] " << ktxPath << " -> " << pngPath;

    ktxTexture* base = nullptr;
    KTX_error_code rc = ktxTexture_CreateFromNamedFile(ktxPath.string().c_str(),
                                                       KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                                       &base);
    if (rc != KTX_SUCCESS || !base) {
        throw AppException("ktxTexture_CreateFromNamedFile failed for " + ktxPath.string());
    }

    // If KTX2 (BasisU), transcode to RGBA8
    if (base->classId == ktxTexture2_c) {
        ktxTexture2* tex2 = reinterpret_cast<ktxTexture2*>(base);
        if (ktxTexture2_NeedsTranscoding(tex2)) {
            rc = ktxTexture2_TranscodeBasis(tex2, KTX_TTF_RGBA32, KTX_TF_HIGH_QUALITY);
            if (rc != KTX_SUCCESS) {
                ktxTexture_Destroy(base);
                throw AppException("ktxTexture2_TranscodeBasis failed for " + ktxPath.string());
            }
        }
    }

    // Get mip 0, layer 0, face 0
    ktx_size_t offset = 0;
    rc = ktxTexture_GetImageOffset(base, /*level*/ 0, /*layer*/ 0, /*faceSlice*/ 0, &offset);
    if (rc != KTX_SUCCESS) {
        ktxTexture_Destroy(base);
        throw AppException("ktxTexture_GetImageOffset failed for " + ktxPath.string());
    }

    const uint8_t* img = static_cast<const uint8_t*>(base->pData) + offset;
    const int w = static_cast<int>(base->baseWidth);
    const int h = static_cast<int>(base->baseHeight);

    // After transcoding to RGBA32, we have 4 channels, 8 bits each
    const int comp = 4;
    const int stride = w * comp;

    fs::create_directories(pngPath.parent_path());
    int ok = stbi_write_png(pngPath.string().c_str(), w, h, comp, img, stride);
    ktxTexture_Destroy(base);

    if (!ok) {
        throw AppException("stbi_write_png failed for " + pngPath.string());
    }
}

/// Replace all *.ktx2 references in .mtl files with *.png (converting files)
/// @param mtlPath Path to the MTL file to patch
/// @throws FSException if file operations fail
static void patchMtlKtx2ToPng(const fs::path& mtlPath) {
    if (!fs::exists(mtlPath))
        return;

    std::ifstream in(mtlPath);
    if (!in) {
        throw FSException("Cannot open MTL file: " + mtlPath.string());
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    in.close();

    auto endsWithKtx2 = [](const std::string& s) {
        if (s.size() < 5)
            return false;
        std::string low = s;
        utils::toLower(low);
        return low.rfind(".ktx2") == low.size() - 5;
    };

    fs::path mtlDir = mtlPath.parent_path();
    std::istringstream lines(content);
    std::ostringstream out;
    std::string line;
    bool changed = false;

    const char* keys[] =
        {"map_Kd", "map_Ks", "map_Bump", "map_d", "map_Pr", "map_Pm", "map_Ps", "map_Ke", "map_Ka"};

    while (std::getline(lines, line)) {
        bool handled = false;
        for (auto* key : keys) {
            if (line.rfind(key, 0) == 0) {
                size_t pos = line.find_first_of(" \t");
                std::string value = (pos == std::string::npos) ? "" : line.substr(pos + 1);
                utils::trim(value);
                if (!value.empty() && endsWithKtx2(value)) {
                    fs::path ktxPath = mtlDir / fs::path(value).lexically_normal();
                    fs::path pngPath = ktxPath;
                    pngPath.replace_extension(".png");
                    try {
                        convertKtx2ToPng(ktxPath, pngPath);
                        std::string newLine = std::string(key) + " " + pngPath.filename().string();
                        out << newLine << "\n";
                        LOGD << "MTL patch: " << key << " -> " << pngPath.filename().string();
                        changed = true;
                        handled = true;
                    } catch (const AppException& e) {
                        LOGW << "KTX2 conversion failed, keeping original reference: " << ktxPath
                             << " (" << e.what() << ")";
                    }
                }
                break;
            }
        }
        if (!handled)
            out << line << "\n";
    }

    if (changed) {
        std::ofstream o(mtlPath, std::ios::trunc);
        if (!o) {
            throw FSException("Cannot write MTL file: " + mtlPath.string());
        }
        o << out.str();
        o.close();
    }
}


/// Export a scene to OBJ or PLY format using Assimp
/// @param scene The Assimp scene to export
/// @param outBaseNoExt Base output path without extension
/// @param forcePLY Force PLY format even if UVs are present
/// @param preferPLYIfNoUV Prefer PLY if no UVs and vertex colors are present
/// @param hasUVs Whether the scene has UV coordinates
/// @param outGeomPath Output parameter for the generated geometry file path
/// @param outMtlPath Output parameter for the generated MTL file path (OBJ only)
/// @throws AppException if export fails
static void exportWithAssimp(const aiScene* scene,
                             const fs::path& outBaseNoExt,
                             bool forcePLY,
                             bool preferPLYIfNoUV,
                             bool hasUVs,
                             std::string& outGeomPath,
                             std::string& outMtlPath) {
    Assimp::Exporter exporter;
    fs::create_directories(outBaseNoExt.parent_path());

    bool usePLY = forcePLY || (!hasUVs && preferPLYIfNoUV);
    const char* fmt = usePLY ? "ply" : "obj";

    fs::path geomPath = outBaseNoExt;
    geomPath.replace_extension(usePLY ? ".ply" : ".obj");

    auto r = exporter.Export(scene, fmt, geomPath.string());
    if (r != AI_SUCCESS) {
        throw AppException("Assimp export failed: " + std::string(exporter.GetErrorString()));
    }

    outGeomPath = geomPath.string();
    outMtlPath.clear();

    if (!usePLY) {
        // .mtl with the same base name next to the OBJ
        fs::path mtl = outBaseNoExt;
        mtl.replace_extension(".mtl");
        if (!fs::exists(mtl)) {
            mtl = geomPath;
            mtl.replace_extension(".mtl");
        }
        if (fs::exists(mtl))
            outMtlPath = mtl.string();
    }

    LOGD << "Exported " << (usePLY ? "PLY: " : "OBJ: ") << outGeomPath;
    if (!outMtlPath.empty())
        LOGD << "MTL: " << outMtlPath;
}


/// Convert glTF/GLB file to OBJ or PLY format
/// @param inputGltf Path to the input glTF or GLB file
/// @param outputBasePath Base path for output files (without extension)
/// @param outGeomPath Output parameter for the generated geometry file path
/// @param outMtlPath Output parameter for the generated MTL file path (OBJ only)
/// @param forcePLY Force PLY format output even if UVs are present
/// @param preferPLYIfNoUV Prefer PLY format if no UVs and vertex colors are present
/// @throws AppException if conversion fails
void convertGltfTo3dModel(const std::string& inputGltf,
                          const std::string& outputBasePath,
                          std::string& outGeomPath,
                          std::string& outMtlPath,
                          bool forcePLY,
                          bool preferPLYIfNoUV) {

    // Import glTF/GLB with standard postprocessing flags
    unsigned pp = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                  aiProcess_ImproveCacheLocality | aiProcess_SortByPType |
                  aiProcess_PreTransformVertices | aiProcess_GenSmoothNormals |
                  aiProcess_CalcTangentSpace;

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(inputGltf, pp);
    if (!scene) {
        throw AppException("Assimp import failed: " + std::string(importer.GetErrorString()));
    }

    bool hasUV = sceneHasUVs(scene);
    bool hasVC = sceneHasVertexColors(scene);
    LOGD << "[assimp] meshes=" << scene->mNumMeshes << " UV=" << (hasUV ? "Y" : "N")
         << " VCols=" << (hasVC ? "Y" : "N");

    // Export geometry
    fs::path outBase(outputBasePath);

    exportWithAssimp(scene,
                     outBase,
                     forcePLY,
                     preferPLYIfNoUV,
                     hasUV,
                     outGeomPath,
                     outMtlPath);

    // For OBJ: patch MTL for KTX2 -> PNG
    if (!outMtlPath.empty()) {
        try {
            patchMtlKtx2ToPng(outMtlPath);
        } catch (const AppException& e) {
            LOGW << "MTL patch failed (continuing anyway): " << e.what();
        }
    }

    LOGD << "glTF/GLB conversion completed: " << outGeomPath;
}

}  // namespace ddb
