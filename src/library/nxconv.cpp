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

namespace nxconv {

//========== Options / Result ===================================================

struct ConvertOptions {
    bool forcePLY = false;
    bool preferPLYIfNoUV = true;  // prefer PLY if no UVs and vertex colors present
    bool makeNXZ = true;
    bool preTransformVertices = true;  // bake transforms (flatten hierarchy)
    bool genNormalsIfMissing = true;
    bool calcTangentsIfUV = true;
    bool keepIntermediates = false;  // if false, cleanup generated OBJ/PLY files
};

struct ConvertResult {
    bool ok = false;
    std::string geomPath;  // generated OBJ or PLY
    std::string mtlPath;
    std::string nxsOrNxz;  // final requested path
    std::string nxsPath;
};

//========== Scene Helpers =========================================================

static bool sceneHasUVs(const aiScene* s) {
    if (!s)
        return false;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        if (s->mMeshes[m]->mTextureCoords[0])
            return true;
    }
    return false;
}

static bool sceneHasVertexColors(const aiScene* s) {
    if (!s)
        return false;
    for (unsigned m = 0; m < s->mNumMeshes; ++m) {
        if (s->mMeshes[m]->mColors[0])
            return true;
    }
    return false;
}

//========== KTX2 -> PNG ===========================================================

static bool convertKtx2ToPng(const fs::path& ktxPath, const fs::path& pngPath) {
    PLOGI << "[ktx2->png] " << ktxPath << " -> " << pngPath;

    ktxTexture* base = nullptr;
    KTX_error_code rc = ktxTexture_CreateFromNamedFile(ktxPath.string().c_str(),
                                                       KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                                       &base);
    if (rc != KTX_SUCCESS || !base) {
        PLOGE << "ktxTexture_CreateFromNamedFile failed";
        return false;
    }

    // If KTX2 (BasisU), transcode to RGBA8
    if (base->classId == ktxTexture2_c) {
        ktxTexture2* tex2 = reinterpret_cast<ktxTexture2*>(base);
        if (ktxTexture2_NeedsTranscoding(tex2)) {
            rc = ktxTexture2_TranscodeBasis(tex2, KTX_TTF_RGBA32, KTX_TF_HIGH_QUALITY);
            if (rc != KTX_SUCCESS) {
                PLOGE << "ktxTexture2_TranscodeBasis failed";
                ktxTexture_Destroy(base);
                return false;
            }
        }
    }

    // Get mip 0, layer 0, face 0
    ktx_size_t offset = 0;
    rc = ktxTexture_GetImageOffset(base, /*level*/ 0, /*layer*/ 0, /*faceSlice*/ 0, &offset);
    if (rc != KTX_SUCCESS) {
        PLOGE << "ktxTexture_GetImageOffset failed";
        ktxTexture_Destroy(base);
        return false;
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
        PLOGE << "stbi_write_png failed";
        return false;
    }
    return true;
}

// Replace all *.ktx2 references in .mtl files with *.png (converting files)
static bool patchMtlKtx2ToPng(const fs::path& mtlPath) {
    if (!fs::exists(mtlPath))
        return true;

    std::ifstream in(mtlPath);
    if (!in) {
        PLOGE << "Cannot open MTL: " << mtlPath;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    in.close();

    auto endsWithKtx2 = [](const std::string& s) {
        if (s.size() < 5)
            return false;
        std::string low = s;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
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
                if (!value.empty() && endsWithKtx2(value)) {
                    fs::path ktxPath = mtlDir / fs::path(value).lexically_normal();
                    fs::path pngPath = ktxPath;
                    pngPath.replace_extension(".png");
                    if (convertKtx2ToPng(ktxPath, pngPath)) {
                        std::string newLine = std::string(key) + " " + pngPath.filename().string();
                        out << newLine << "\n";
                        PLOGI << "MTL patch: " << key << " -> " << pngPath.filename().string();
                        changed = true;
                        handled = true;
                    } else {
                        PLOGW << "KTX2 conversion failed, keeping original reference: " << ktxPath;
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
        o << out.str();
        o.close();
    }
    return true;
}

//========== Assimp Export (OBJ/PLY) ===============================================

static bool exportWithAssimp(const aiScene* scene,
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
        PLOGE << "Assimp Export failed: " << exporter.GetErrorString();
        return false;
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

    PLOGI << "Exported " << (usePLY ? "PLY: " : "OBJ: ") << outGeomPath;
    if (!outMtlPath.empty())
        PLOGI << "MTL: " << outMtlPath;
    return true;
}

//========== Public Function =====================================================

ConvertResult ConvertGltfGlbToNexus(const std::string& inputGltf,
                                    const std::string& outputNxsOrNxz,
                                    const ConvertOptions& opt = {},
                                    std::string* errMsgOut = nullptr) {
    ConvertResult R;
    R.nxsOrNxz = outputNxsOrNxz;

    // Import glTF/GLB
    unsigned pp = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                  aiProcess_ImproveCacheLocality | aiProcess_SortByPType;

    if (opt.preTransformVertices)
        pp |= aiProcess_PreTransformVertices;
    if (opt.genNormalsIfMissing)
        pp |= aiProcess_GenSmoothNormals;
    if (opt.calcTangentsIfUV)
        pp |= aiProcess_CalcTangentSpace;

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(inputGltf, pp);
    if (!scene) {
        std::string msg = std::string("Assimp import failed: ") + importer.GetErrorString();
        PLOGE << msg;
        if (errMsgOut)
            *errMsgOut = msg;
        return R;
    }

    bool hasUV = sceneHasUVs(scene);
    bool hasVC = sceneHasVertexColors(scene);
    PLOGI << "[assimp] meshes=" << scene->mNumMeshes << " UV=" << (hasUV ? "Y" : "N")
          << " VCols=" << (hasVC ? "Y" : "N");

    // Export geometry
    fs::path outPath(outputNxsOrNxz);
    fs::path outBase = outPath;
    outBase.replace_extension("");  // base path without extension

    if (!exportWithAssimp(scene,
                          outBase,
                          opt.forcePLY,
                          opt.preferPLYIfNoUV,
                          hasUV,
                          R.geomPath,
                          R.mtlPath)) {
        if (errMsgOut)
            *errMsgOut = "Geometry export failed";
        return R;
    }

    // For OBJ: patch MTL for KTX2 -> PNG
    if (!R.mtlPath.empty()) {
        if (!patchMtlKtx2ToPng(R.mtlPath)) {
            PLOGW << "MTL patch failed (continuing anyway)";
        }
    }

    // Nexus build/compress in-process
    char nerr[2048] = {0};
    PLOGI << "Running nexusBuild: input=" << R.geomPath << " output=" << outputNxsOrNxz;
    NXSErr rc = nexusBuild(R.geomPath.c_str(), outputNxsOrNxz.c_str(), nerr, sizeof(nerr));
    if (rc != NXSERR_NONE) {
        std::string msg =
            std::string("nexusBuild failed (code=") + std::to_string((int)rc) + "): " + nerr;
        PLOGE << msg;
        if (errMsgOut)
            *errMsgOut = msg;
        return R;
    }

    // Cleanup intermediates if requested
    if (!opt.keepIntermediates) {
        try {
            if (!R.mtlPath.empty() && fs::exists(R.mtlPath))
                fs::remove(R.mtlPath);
            if (!R.geomPath.empty() && fs::exists(R.geomPath))
                fs::remove(R.geomPath);
        } catch (...) {
            PLOGW << "Intermediate cleanup failed (ignoring).";
        }
    }

    R.ok = true;
    PLOGI << "Conversion completed: " << outputNxsOrNxz;
    return R;
}

}  // namespace nxconv
