/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "3d.h"
#include "build.h"
#include "dbops.h"
#include "entry.h"
#include "test.h"
#include "testfs.h"
#include "testarea.h"
#include "utils.h"
#include <cpr/cpr.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include "exceptions.h"
#include <tiny_obj_loader.h>

namespace
{

    using namespace ddb;

    // Minimal base64 encoder for embedding buffer bytes as glTF data URIs in tests.
    std::string toBase64(const std::vector<uint8_t>& data) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < data.size(); i += 3) {
            uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            out += table[(n >> 18) & 0x3F];
            out += table[(n >> 12) & 0x3F];
            out += table[(n >> 6) & 0x3F];
            out += table[n & 0x3F];
        }
        const size_t rem = data.size() - i;
        if (rem == 1) {
            uint32_t n = data[i] << 16;
            out += table[(n >> 18) & 0x3F];
            out += table[(n >> 12) & 0x3F];
            out += "==";
        } else if (rem == 2) {
            uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
            out += table[(n >> 18) & 0x3F];
            out += table[(n >> 12) & 0x3F];
            out += table[(n >> 6) & 0x3F];
            out += "=";
        }
        return out;
    }

    std::string dataUri(const std::vector<uint8_t>& data) {
        return "data:application/octet-stream;base64," + toBase64(data);
    }

    template <typename T>
    std::vector<uint8_t> toBytes(const std::vector<T>& values) {
        std::vector<uint8_t> out(values.size() * sizeof(T));
        std::memcpy(out.data(), values.data(), out.size());
        return out;
    }

    // Wraps a glTF JSON string in a minimal, spec-valid GLB container (single JSON chunk).
    void writeGlb(const fs::path& path, const std::string& json) {
        std::ofstream out(path.string(), std::ios::binary);
        const uint32_t magic = 0x46546C67; // "glTF"
        const uint32_t version = 2;
        const uint32_t jsonLen = static_cast<uint32_t>(json.size());
        const uint32_t totalLen = 12 + 8 + jsonLen;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&totalLen), 4);
        out.write(reinterpret_cast<const char*>(&jsonLen), 4);
        const uint32_t chunkType = 0x4E4F534A; // "JSON"
        out.write(reinterpret_cast<const char*>(&chunkType), 4);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
    }

    // Writes the GLB layout exporters actually emit: a space-padded JSON chunk followed
    // by a zero-padded BIN chunk, both 4-byte aligned. declaredTotalLen overrides the
    // header's length field when non-zero, to exercise inconsistent-but-readable files.
    void writeGlbWithBin(const fs::path& path,
                         const std::string& json,
                         const std::vector<uint8_t>& bin,
                         uint32_t declaredTotalLen = 0) {
        std::string paddedJson = json;
        while (paddedJson.size() % 4 != 0)
            paddedJson += ' ';

        std::vector<uint8_t> paddedBin = bin;
        while (paddedBin.size() % 4 != 0)
            paddedBin.push_back(0);

        const uint32_t jsonLen = static_cast<uint32_t>(paddedJson.size());
        const uint32_t binLen = static_cast<uint32_t>(paddedBin.size());
        const uint32_t realTotalLen = 12 + 8 + jsonLen + 8 + binLen;

        std::ofstream out(path.string(), std::ios::binary);
        const uint32_t magic = 0x46546C67;    // "glTF"
        const uint32_t version = 2;
        const uint32_t totalLen = declaredTotalLen != 0 ? declaredTotalLen : realTotalLen;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&totalLen), 4);

        const uint32_t jsonChunkType = 0x4E4F534A; // "JSON"
        out.write(reinterpret_cast<const char*>(&jsonLen), 4);
        out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
        out.write(paddedJson.data(), static_cast<std::streamsize>(paddedJson.size()));

        const uint32_t binChunkType = 0x004E4942; // "BIN\0"
        out.write(reinterpret_cast<const char*>(&binLen), 4);
        out.write(reinterpret_cast<const char*>(&binChunkType), 4);
        out.write(reinterpret_cast<const char*>(paddedBin.data()),
                  static_cast<std::streamsize>(paddedBin.size()));
    }

    // A single-triangle asset whose buffer has no uri: its bytes live in the GLB BIN chunk.
    std::string glbTriangleGltfJson(size_t binLength, const char* modeField) {
        std::ostringstream j;
        j << "{"
             "\"asset\":{\"version\":\"2.0\"},"
             "\"buffers\":[{\"byteLength\":" << binLength << "}],"
             "\"bufferViews\":["
             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
             "],"
             "\"accessors\":["
             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},"
             "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
             "],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1"
          << (modeField ? std::string(",\"mode\":") + modeField : std::string())
          << "}]}],"
             "\"nodes\":[{\"mesh\":0}],"
             "\"scenes\":[{\"nodes\":[0]}],"
             "\"scene\":0"
             "}";
        return j.str();
    }

    // Positions + indices laid out to match glbTriangleGltfJson's bufferViews.
    std::vector<uint8_t> glbTriangleBin() {
        const std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::vector<uint16_t> indices = {0, 1, 2};
        std::vector<uint8_t> bin = toBytes(positions);
        const std::vector<uint8_t> idx = toBytes(indices);
        bin.insert(bin.end(), idx.begin(), idx.end());
        return bin;
    }

    // A single-triangle glTF asset, "mode" of the sole primitive is caller-controlled.
    std::string triangleGltfJson(const char* modeField) {
        const std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::vector<uint16_t> indices = {0, 1, 2};
        std::ostringstream j;
        j << "{"
             "\"asset\":{\"version\":\"2.0\"},"
             "\"buffers\":["
             "{\"uri\":\"" << dataUri(toBytes(positions)) << "\",\"byteLength\":" << (positions.size() * 4) << "},"
             "{\"uri\":\"" << dataUri(toBytes(indices)) << "\",\"byteLength\":" << (indices.size() * 2) << "}"
             "],"
             "\"bufferViews\":["
             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << (positions.size() * 4) << "},"
             "{\"buffer\":1,\"byteOffset\":0,\"byteLength\":" << (indices.size() * 2) << "}"
             "],"
             "\"accessors\":["
             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},"
             "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
             "],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1"
          << (modeField ? std::string(",\"mode\":") + modeField : std::string())
          << "}]}],"
             "\"nodes\":[{\"mesh\":0}],"
             "\"scenes\":[{\"nodes\":[0]}],"
             "\"scene\":0"
             "}";
        return j.str();
    }

    // A glTF asset with two primitives in the same mesh: one triangle (mode 4, no UVs)
    // and one point cloud (mode 0), mirroring a splat tile shipped alongside a proxy mesh.
    std::string mixedTrianglePointGltfJson() {
        const std::vector<float> triPositions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::vector<uint16_t> indices = {0, 1, 2};
        const std::vector<float> ptPositions = {5, 5, 5};
        std::ostringstream j;
        j << "{"
             "\"asset\":{\"version\":\"2.0\"},"
             "\"buffers\":["
             "{\"uri\":\"" << dataUri(toBytes(triPositions)) << "\",\"byteLength\":" << (triPositions.size() * 4) << "},"
             "{\"uri\":\"" << dataUri(toBytes(indices)) << "\",\"byteLength\":" << (indices.size() * 2) << "},"
             "{\"uri\":\"" << dataUri(toBytes(ptPositions)) << "\",\"byteLength\":" << (ptPositions.size() * 4) << "}"
             "],"
             "\"bufferViews\":["
             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << (triPositions.size() * 4) << "},"
             "{\"buffer\":1,\"byteOffset\":0,\"byteLength\":" << (indices.size() * 2) << "},"
             "{\"buffer\":2,\"byteOffset\":0,\"byteLength\":" << (ptPositions.size() * 4) << "}"
             "],"
             "\"accessors\":["
             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},"
             "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
             "{\"bufferView\":2,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\",\"max\":[5,5,5],\"min\":[5,5,5]}"
             "],"
             "\"meshes\":[{\"primitives\":["
             "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":4},"
             "{\"attributes\":{\"POSITION\":2},\"mode\":0}"
             "]}],"
             "\"nodes\":[{\"mesh\":0}],"
             "\"scenes\":[{\"nodes\":[0]}],"
             "\"scene\":0"
             "}";
        return j.str();
    }

    // Helper function to verify OBJ file content and MTL texture references using tinyobj
    void verifyObjAndTextures(const std::string& outGeomPath, const std::string& outMtlPath) {
        // Verify OBJ file is readable and has valid content
        std::ifstream objFile(outGeomPath);
        ASSERT_TRUE(objFile.is_open()) << "Failed to open OBJ file: " << outGeomPath;
        std::string line;
        bool hasVertices = false;
        bool hasFaces = false;
        while (std::getline(objFile, line)) {
            if (line.rfind("v ", 0) == 0) hasVertices = true;
            if (line.rfind("f ", 0) == 0) hasFaces = true;
        }
        objFile.close();
        ASSERT_TRUE(hasVertices) << "OBJ file has no vertices";
        ASSERT_TRUE(hasFaces) << "OBJ file has no faces";

        // If MTL exists, verify it's readable and check for texture references using tinyobj
        if (!outMtlPath.empty()) {
            tinyobj::ObjReader reader;
            tinyobj::ObjReaderConfig config;
            config.mtl_search_path = fs::path(outGeomPath).parent_path().string();

            ASSERT_TRUE(reader.ParseFromFile(outGeomPath, config))
                << "Failed to parse OBJ with tinyobj: " << reader.Error();

            const auto& materials = reader.GetMaterials();
            fs::path mtlDir = fs::path(outMtlPath).parent_path();

            for (const auto& material : materials) {
                // Check all texture maps
                std::vector<std::string> textureMaps = {
                    material.diffuse_texname,
                    material.specular_texname,
                    material.bump_texname,
                    material.displacement_texname,
                    material.alpha_texname,
                    material.reflection_texname,
                    material.roughness_texname,
                    material.metallic_texname,
                    material.sheen_texname,
                    material.emissive_texname,
                    material.normal_texname
                };

                for (const auto& texturePath : textureMaps) {
                    if (!texturePath.empty()) {
                        fs::path fullTexturePath = mtlDir / texturePath;
                        ASSERT_TRUE(fs::exists(fullTexturePath))
                            << "Texture file not found: " << fullTexturePath.string()
                            << " referenced in MTL: " << outMtlPath
                            << " for material: " << material.name;
                        std::cout << "  Verified texture: " << texturePath << std::endl;
                    }
                }
            }
        }
    }

    TEST(file3d, odmGetDependencies)
    {
        try
        {
            // URL of the test archive
            std::string archiveUrl = testDataUrl("3d/odm_texturing.zip");

            // Create an instance of TestFS
            TestFS testFS(archiveUrl, "odm_texturing", true);

            auto dependencies = getObjDependencies("odm_textured_model_geo.obj");

            ASSERT_EQ(dependencies.size(), 3);
            ASSERT_EQ(dependencies[0], "odm_textured_model_geo.mtl");
            ASSERT_EQ(dependencies[1], "odm_textured_model_geo_material0000_map_Kd.jpg");
            ASSERT_EQ(dependencies[2], "odm_textured_model_geo_material0001_map_Kd.jpg");

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, odmLeadingSpacesGetDependencies)
    {
        try
        {
            // URL of the test archive
            std::string archiveUrl = testDataUrl("3d/odm_texturing_leading_spaces.zip");

            // Create an instance of TestFS
            TestFS testFS(archiveUrl, "odm_texturing_leading_spaces", true);

            auto dependencies = getObjDependencies("odm_textured_model_geo.obj");

            ASSERT_EQ(dependencies.size(), 3);
            ASSERT_EQ(dependencies[0], "odm_textured_model_geo.mtl");
            ASSERT_EQ(dependencies[1], "odm_textured_model_geo_material0000_map_Kd.jpg");
            ASSERT_EQ(dependencies[2], "odm_textured_model_geo_material0001_map_Kd.jpg");

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, odmMultipleGetDependencies)
    {
        try
        {
            // URL of the test archive
            std::string archiveUrl = testDataUrl("3d/odm_texturing_multiple.zip");

            // Create an instance of TestFS
            TestFS testFS(archiveUrl, "odm_texturing_multiple", true);

            auto dependencies = getObjDependencies("odm_textured_model_geo.obj");

            ASSERT_EQ(dependencies.size(), 23);
            ASSERT_EQ(dependencies[0], "odm_textured_model_geo.mtl");
            ASSERT_EQ(dependencies[1], "odm_textured_model_geo_material0000_map_Kd.png");
            ASSERT_EQ(dependencies[2], "odm_textured_model_geo_material0001_map_Kd.png");
            ASSERT_EQ(dependencies[3], "odm_textured_model_geo_material0002_map_Kd.png");
            ASSERT_EQ(dependencies[4], "odm_textured_model_geo_material0003_map_Kd.png");
            ASSERT_EQ(dependencies[5], "odm_textured_model_geo_material0004_map_Kd.png");
            ASSERT_EQ(dependencies[6], "odm_textured_model_geo_material0005_map_Kd.png");
            ASSERT_EQ(dependencies[7], "odm_textured_model_geo_material0006_map_Kd.png");
            ASSERT_EQ(dependencies[8], "odm_textured_model_geo_material0007_map_Kd.png");
            ASSERT_EQ(dependencies[9], "odm_textured_model_geo_material0008_map_Kd.png");
            ASSERT_EQ(dependencies[10], "odm_textured_model_geo_material0009_map_Kd.png");
            ASSERT_EQ(dependencies[11], "odm_textured_model_geo_material0010_map_Kd.png");
            ASSERT_EQ(dependencies[12], "odm_textured_model_geo_material0011_map_Kd.png");
            ASSERT_EQ(dependencies[13], "odm_textured_model_geo_material0012_map_Kd.png");
            ASSERT_EQ(dependencies[14], "odm_textured_model_geo_material0013_map_Kd.png");
            ASSERT_EQ(dependencies[15], "odm_textured_model_geo_material0014_map_Kd.png");
            ASSERT_EQ(dependencies[16], "odm_textured_model_geo_material0015_map_Kd.png");
            ASSERT_EQ(dependencies[17], "odm_textured_model_geo_material0016_map_Kd.png");
            ASSERT_EQ(dependencies[18], "odm_textured_model_geo_material0017_map_Kd.png");
            ASSERT_EQ(dependencies[19], "odm_textured_model_geo_material0018_map_Kd.png");
            ASSERT_EQ(dependencies[20], "odm_textured_model_geo_material0019_map_Kd.png");
            ASSERT_EQ(dependencies[21], "odm_textured_model_geo_material0020_map_Kd.png");
            ASSERT_EQ(dependencies[22], "odm_textured_model_geo_material0021_map_Kd.png");

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, metashapeGetDependencies)
    {
        try
        {
            // URL of the test archive
            std::string archiveUrl = testDataUrl("3d/metashape_obj.zip");

            // Create an instance of TestFS
            TestFS testFS(archiveUrl, "metashape_obj", true);

            auto dependencies = getObjDependencies("brighton_beach.obj");

            ASSERT_EQ(dependencies.size(), 2);
            ASSERT_EQ(dependencies[0], "brighton_beach.mtl");
            ASSERT_EQ(dependencies[1], "brighton_beach.jpg");

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, metashapeWithSpacesGetDependencies)
    {
        try
        {
            // URL of the test archive
            std::string archiveUrl = testDataUrl("3d/metashape_obj_with_spaces.zip");

            // Create an instance of TestFS
            TestFS testFS(archiveUrl, "metashape_obj_with_spaces", true);

            auto dependencies = getObjDependencies("brighton beach.obj");

            ASSERT_EQ(dependencies.size(), 2);
            ASSERT_EQ(dependencies[0], "brighton beach.mtl");
            ASSERT_EQ(dependencies[1], "brighton beach.jpg");

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, convertGltfToObjTest)
    {
        try
        {
            // URL of the test archive containing model.gltf and model.bin
            std::string archiveUrl = testDataUrl("3d/model-gltf.zip");

            // Create an instance of TestFS
            TestFS testFS(archiveUrl, "model-gltf", true);

            // Output paths
            std::string outGeomPath;
            std::string outMtlPath;

            // Create absolute path for output
            fs::path outputBasePath = fs::path(testFS.testFolder) / "output_model";

            // Convert GLTF to 3D model (OBJ/PLY)
            convertGltfTo3dModel("model.gltf", outputBasePath.string(), outGeomPath, outMtlPath, false, true);

            // Verify that output files were created
            ASSERT_FALSE(outGeomPath.empty());
            ASSERT_TRUE(fs::exists(outGeomPath));

            // If MTL was created (OBJ format), verify it exists
            if (!outMtlPath.empty()) {
                ASSERT_TRUE(fs::exists(outMtlPath));
            }

            std::cout << "Generated geometry file: " << outGeomPath << std::endl;
            if (!outMtlPath.empty()) {
                std::cout << "Generated MTL file: " << outMtlPath << std::endl;
            }

            verifyObjAndTextures(outGeomPath, outMtlPath);

            std::cout << "All files verified successfully!" << std::endl;

            // Test nexus conversion from the converted OBJ
            fs::path nexusOutput = fs::path(testFS.testFolder) / "model_from_gltf.nxz";
            std::string nexusPath = buildNexus(outGeomPath, nexusOutput.string(), true);

            // Verify nexus file was created
            ASSERT_FALSE(nexusPath.empty());
            ASSERT_TRUE(fs::exists(nexusPath));
            ASSERT_GT(fs::file_size(nexusPath), 0);

            std::cout << "Successfully created nexus file: " << nexusPath << std::endl;
            std::cout << "Nexus file size: " << fs::file_size(nexusPath) << " bytes" << std::endl;

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, convertGlbToObjTest)
    {
        try
        {
            // URL of the test GLB file
            std::string glbUrl = testDataUrl("3d/model.glb");

            // Create an instance of TestArea
            TestArea testArea("model-glb");
            auto glbFile = testArea.downloadTestAsset(glbUrl, "model.glb");

            // Output paths
            std::string outGeomPath;
            std::string outMtlPath;

            // Create absolute path for output
            fs::path outputBasePath = testArea.getPath("output_model_glb");

            // Convert GLB to 3D model (OBJ/PLY)
            convertGltfTo3dModel(glbFile.string(), outputBasePath.string(), outGeomPath, outMtlPath, false, true);

            // Verify that output files were created
            ASSERT_FALSE(outGeomPath.empty());
            ASSERT_TRUE(fs::exists(outGeomPath));

            // If MTL was created (OBJ format), verify it exists
            if (!outMtlPath.empty()) {
                ASSERT_TRUE(fs::exists(outMtlPath));
            }

            std::cout << "Generated geometry file: " << outGeomPath << std::endl;
            if (!outMtlPath.empty()) {
                std::cout << "Generated MTL file: " << outMtlPath << std::endl;
            }

            verifyObjAndTextures(outGeomPath, outMtlPath);

            std::cout << "All files verified successfully!" << std::endl;

            // Test nexus conversion from the converted OBJ
            fs::path nexusOutput = testArea.getPath("model_from_glb.nxz");
            std::string nexusPath = buildNexus(outGeomPath, nexusOutput.string(), true);

            // Verify nexus file was created
            ASSERT_FALSE(nexusPath.empty());
            ASSERT_TRUE(fs::exists(nexusPath));
            ASSERT_GT(fs::file_size(nexusPath), 0);

            std::cout << "Successfully created nexus file: " << nexusPath << std::endl;
            std::cout << "Nexus file size: " << fs::file_size(nexusPath) << " bytes" << std::endl;

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, convertSunglassesGlbTest)
    {
        try
        {
            // URL of the test GLB file
            std::string glbUrl = testDataUrl("3d/SunglassesKhronos.glb");

            // Create an instance of TestArea
            TestArea testArea("sunglasses-glb");
            auto glbFile = testArea.downloadTestAsset(glbUrl, "SunglassesKhronos.glb");

            // Output paths
            std::string outGeomPath;
            std::string outMtlPath;

            // Create absolute path for output
            fs::path outputBasePath = testArea.getPath("output_sunglasses");

            // Convert GLB to 3D model (OBJ/PLY)
            convertGltfTo3dModel(glbFile.string(), outputBasePath.string(), outGeomPath, outMtlPath, false, true);

            // Verify that output files were created
            ASSERT_FALSE(outGeomPath.empty());
            ASSERT_TRUE(fs::exists(outGeomPath));

            // If MTL was created (OBJ format), verify it exists
            if (!outMtlPath.empty()) {
                ASSERT_TRUE(fs::exists(outMtlPath));
            }

            std::cout << "Generated geometry file: " << outGeomPath << std::endl;
            if (!outMtlPath.empty()) {
                std::cout << "Generated MTL file: " << outMtlPath << std::endl;
            }

            verifyObjAndTextures(outGeomPath, outMtlPath);

            std::cout << "All files verified successfully!" << std::endl;

            // Test nexus conversion from the converted OBJ
            fs::path nexusOutput = testArea.getPath("sunglasses.nxz");
            std::string nexusPath = buildNexus(outGeomPath, nexusOutput.string(), true);

            // Verify nexus file was created
            ASSERT_FALSE(nexusPath.empty());
            ASSERT_TRUE(fs::exists(nexusPath));
            ASSERT_GT(fs::file_size(nexusPath), 0);

            std::cout << "Successfully created nexus file: " << nexusPath << std::endl;
            std::cout << "Nexus file size: " << fs::file_size(nexusPath) << " bytes" << std::endl;

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, convertIridescentDishGlbTest)
    {
        try
        {
            // URL of the test GLB file
            std::string glbUrl = testDataUrl("3d/IridescentDishWithOlives.glb");

            // Create an instance of TestArea
            TestArea testArea("dish-glb");
            auto glbFile = testArea.downloadTestAsset(glbUrl, "IridescentDishWithOlives.glb");

            // Output paths
            std::string outGeomPath;
            std::string outMtlPath;

            // Create absolute path for output
            fs::path outputBasePath = testArea.getPath("output_dish");

            // Convert GLB to 3D model (OBJ/PLY)
            convertGltfTo3dModel(glbFile.string(), outputBasePath.string(), outGeomPath, outMtlPath, false, true);

            // Verify that output files were created
            ASSERT_FALSE(outGeomPath.empty());
            ASSERT_TRUE(fs::exists(outGeomPath));

            // If MTL was created (OBJ format), verify it exists
            if (!outMtlPath.empty()) {
                ASSERT_TRUE(fs::exists(outMtlPath));
            }

            std::cout << "Generated geometry file: " << outGeomPath << std::endl;
            if (!outMtlPath.empty()) {
                std::cout << "Generated MTL file: " << outMtlPath << std::endl;
            }

            verifyObjAndTextures(outGeomPath, outMtlPath);

            std::cout << "All files verified successfully!" << std::endl;

            // Test nexus conversion from the converted OBJ
            fs::path nexusOutput = testArea.getPath("dish.nxz");
            std::string nexusPath = buildNexus(outGeomPath, nexusOutput.string(), true);

            // Verify nexus file was created
            ASSERT_FALSE(nexusPath.empty());
            ASSERT_TRUE(fs::exists(nexusPath));
            ASSERT_GT(fs::file_size(nexusPath), 0);

            std::cout << "Successfully created nexus file: " << nexusPath << std::endl;
            std::cout << "Nexus file size: " << fs::file_size(nexusPath) << " bytes" << std::endl;

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, convertToyCarGlbTest)
    {
        try
        {
            // URL of the test GLB file
            std::string glbUrl = testDataUrl("3d/ToyCar.glb");

            // Create an instance of TestArea
            TestArea testArea("toycar-glb");
            auto glbFile = testArea.downloadTestAsset(glbUrl, "ToyCar.glb");

            // Output paths
            std::string outGeomPath;
            std::string outMtlPath;

            // Create absolute path for output
            fs::path outputBasePath = testArea.getPath("output_toycar");

            // Convert GLB to 3D model (OBJ/PLY)
            convertGltfTo3dModel(glbFile.string(), outputBasePath.string(), outGeomPath, outMtlPath, false, true);

            // Verify that output files were created
            ASSERT_FALSE(outGeomPath.empty());
            ASSERT_TRUE(fs::exists(outGeomPath));

            // If MTL was created (OBJ format), verify it exists
            if (!outMtlPath.empty()) {
                ASSERT_TRUE(fs::exists(outMtlPath));
            }

            std::cout << "Generated geometry file: " << outGeomPath << std::endl;
            if (!outMtlPath.empty()) {
                std::cout << "Generated MTL file: " << outMtlPath << std::endl;
            }

            verifyObjAndTextures(outGeomPath, outMtlPath);

            std::cout << "All files verified successfully!" << std::endl;

            // Test nexus conversion from the converted OBJ
            fs::path nexusOutput = testArea.getPath("toycar.nxz");
            std::string nexusPath = buildNexus(outGeomPath, nexusOutput.string(), true);

            // Verify nexus file was created
            ASSERT_FALSE(nexusPath.empty());
            ASSERT_TRUE(fs::exists(nexusPath));
            ASSERT_GT(fs::file_size(nexusPath), 0);

            std::cout << "Successfully created nexus file: " << nexusPath << std::endl;
            std::cout << "Nexus file size: " << fs::file_size(nexusPath) << " bytes" << std::endl;

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    TEST(file3d, convertDracoGlbTest)
    {
        try
        {
            // URL of the test GLB file with Draco mesh compression
            std::string glbUrl = testDataUrl("3d/draco_model.glb");

            // Create an instance of TestArea
            TestArea testArea("draco-glb");
            auto glbFile = testArea.downloadTestAsset(glbUrl, "draco_model.glb");

            // Output paths
            std::string outGeomPath;
            std::string outMtlPath;

            // Create absolute path for output
            fs::path outputBasePath = testArea.getPath("output_draco");

            // Convert GLB with Draco compression to 3D model (OBJ/PLY)
            convertGltfTo3dModel(glbFile.string(), outputBasePath.string(), outGeomPath, outMtlPath, false, true);

            // Verify that output files were created
            ASSERT_FALSE(outGeomPath.empty());
            ASSERT_TRUE(fs::exists(outGeomPath));

            // If MTL was created (OBJ format), verify it exists
            if (!outMtlPath.empty()) {
                ASSERT_TRUE(fs::exists(outMtlPath));
            }

            std::cout << "Generated geometry file: " << outGeomPath << std::endl;
            if (!outMtlPath.empty()) {
                std::cout << "Generated MTL file: " << outMtlPath << std::endl;
            }

            verifyObjAndTextures(outGeomPath, outMtlPath);

            std::cout << "All files verified successfully!" << std::endl;

            // Test nexus conversion from the converted OBJ
            fs::path nexusOutput = testArea.getPath("draco_model.nxz");
            std::string nexusPath = buildNexus(outGeomPath, nexusOutput.string(), true);

            // Verify nexus file was created
            ASSERT_FALSE(nexusPath.empty());
            ASSERT_TRUE(fs::exists(nexusPath));
            ASSERT_GT(fs::file_size(nexusPath), 0);

            std::cout << "Successfully created nexus file: " << nexusPath << std::endl;
            std::cout << "Nexus file size: " << fs::file_size(nexusPath) << " bytes" << std::endl;

        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
            FAIL();
        }
    }

    // ---------------------------------------------------------------------------
    // identifyGltf() / fingerprint() classification
    // ---------------------------------------------------------------------------

    TEST(file3d, identifyGltfTriangleModeOmittedIsModel)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("triangle.gltf");
        writeFile(gltf, triangleGltfJson(nullptr));
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Model);
    }

    TEST(file3d, identifyGltfTriangleStripAndFanAreModel)
    {
        TestArea ta(TEST_NAME);
        const fs::path strip = ta.getPath("strip.gltf");
        writeFile(strip, triangleGltfJson("5"));
        EXPECT_EQ(ddb::fingerprint(strip), EntryType::Model);

        const fs::path fan = ta.getPath("fan.gltf");
        writeFile(fan, triangleGltfJson("6"));
        EXPECT_EQ(ddb::fingerprint(fan), EntryType::Model);
    }

    TEST(file3d, identifyGltfPointModeIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("points.gltf");
        writeFile(gltf, triangleGltfJson("0"));
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Generic);
    }

    TEST(file3d, identifyGltfLineModesAreGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path lines = ta.getPath("lines.gltf");
        writeFile(lines, triangleGltfJson("1"));
        EXPECT_EQ(ddb::fingerprint(lines), EntryType::Generic);
    }

    TEST(file3d, identifyGltfMalformedJsonIsGenericAndDoesNotThrow)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("garbage.gltf");
        writeFile(gltf, "{ this is not valid json ");
        EntryType type = EntryType::Undefined;
        ASSERT_NO_THROW(type = ddb::fingerprint(gltf));
        EXPECT_EQ(type, EntryType::Generic);
    }

    TEST(file3d, identifyGltfEmptyMeshesArrayIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("nomeshes.gltf");
        writeFile(gltf, "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[]}");
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Generic);
    }

    // glTF 1.0 declares "meshes" as an object keyed by mesh id rather than an array.
    TEST(file3d, identifyGltf1DictMeshesIsModel)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("gltf1.gltf");
        writeFile(gltf,
            "{\"asset\":{\"version\":\"1.0\"},"
            "\"meshes\":{\"mesh0\":{\"primitives\":[{\"attributes\":{\"POSITION\":\"a\"},\"mode\":4}]}}}");
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Model);
    }

    TEST(file3d, identifyGlbTriangleIsModel)
    {
        TestArea ta(TEST_NAME);
        const fs::path glb = ta.getPath("triangle.glb");
        writeGlb(glb, triangleGltfJson(nullptr));
        EXPECT_EQ(ddb::fingerprint(glb), EntryType::Model);
    }

    TEST(file3d, identifyGlbPointOnlyIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path glb = ta.getPath("points.glb");
        writeGlb(glb, triangleGltfJson("0"));
        EXPECT_EQ(ddb::fingerprint(glb), EntryType::Generic);
    }

    // A corrupt GLB declaring a JSON chunk length larger than the file is rejected
    // by the bounds check in readGlbJson() rather than allocating up to 4 GiB.
    TEST(file3d, identifyGlbCorruptChunkLengthIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path glb = ta.getPath("corrupt.glb");

        std::ofstream out(glb.string(), std::ios::binary);
        const uint32_t magic = 0x46546C67;
        const uint32_t version = 2;
        const uint32_t totalLen = 20; // header + chunk header only, no payload
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&totalLen), 4);
        const uint32_t bogusChunkLen = 0xFFFFFFF0u; // declares ~4 GiB of JSON
        const uint32_t chunkType = 0x4E4F534A;
        out.write(reinterpret_cast<const char*>(&bogusChunkLen), 4);
        out.write(reinterpret_cast<const char*>(&chunkType), 4);
        out.close();

        EntryType type = EntryType::Undefined;
        ASSERT_NO_THROW(type = ddb::fingerprint(glb));
        EXPECT_EQ(type, EntryType::Generic);
    }

    // The layout real exporters emit: padded JSON chunk followed by a BIN chunk.
    TEST(file3d, identifyGlbWithBinChunkIsModel)
    {
        TestArea ta(TEST_NAME);
        const auto bin = glbTriangleBin();

        const fs::path model = ta.getPath("triangle_bin.glb");
        writeGlbWithBin(model, glbTriangleGltfJson(bin.size(), nullptr), bin);
        EXPECT_EQ(ddb::fingerprint(model), EntryType::Model);

        const fs::path points = ta.getPath("points_bin.glb");
        writeGlbWithBin(points, glbTriangleGltfJson(bin.size(), "0"), bin);
        EXPECT_EQ(ddb::fingerprint(points), EntryType::Generic);
    }

    // Some exporters overstate the header's total length while the JSON chunk is intact.
    // The allocation is bounded by the chunk length, so such a file stays classifiable.
    TEST(file3d, identifyGlbOverstatedTotalLengthIsStillClassified)
    {
        TestArea ta(TEST_NAME);
        const auto bin = glbTriangleBin();
        const fs::path glb = ta.getPath("overstated.glb");

        writeGlbWithBin(glb, glbTriangleGltfJson(bin.size(), nullptr), bin, 1024 * 1024);
        ASSERT_LT(fs::file_size(glb), 1024u * 1024u);
        EXPECT_EQ(ddb::fingerprint(glb), EntryType::Model);
    }

    // Extension matching is case-insensitive, so classification must run either way.
    TEST(file3d, identifyGltfUppercaseExtensionsAreClassified)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("POINTS.GLTF");
        writeFile(gltf, triangleGltfJson("0"));
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Generic);

        const fs::path glb = ta.getPath("TRIANGLE.GLB");
        writeGlb(glb, triangleGltfJson(nullptr));
        EXPECT_EQ(ddb::fingerprint(glb), EntryType::Model);
    }

    // A primitive that is not an object must not inherit the "mode omitted" triangle
    // default, otherwise a malformed asset would be declared buildable.
    TEST(file3d, identifyGltfNonObjectPrimitiveIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("badprimitive.gltf");
        writeFile(gltf,
            "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primitives\":[\"nonsense\",42,null]}]}");
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Generic);
    }

    // Regression test: a mesh mixing a triangle primitive with a point primitive must not
    // reach libnexus' triangle-soup PLY reader with desynchronized 1-index face records.
    // Without AI_CONFIG_PP_SBP_REMOVE in convertGltfTo3dModel, this used to throw
    // "Bad index in triangle list" (or worse) deep inside nexusBuild.
    TEST(file3d, buildNexusMixedTrianglePointDoesNotCrash)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("mixed.gltf");
        writeFile(gltf, mixedTrianglePointGltfJson());

        // The classifier only requires one triangle primitive to declare the asset a Model.
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Model);

        const fs::path nxz = ta.getPath("mixed.nxz");
        std::string outNxz;
        ASSERT_NO_THROW(outNxz = buildNexus(gltf.string(), nxz.string(), true));
        ASSERT_TRUE(fs::exists(outNxz));
        EXPECT_GT(fs::file_size(outNxz), 0);
    }

    // ---------------------------------------------------------------------------
    // glTF inside a dataset (P3): the original glTF is validated against the dataset
    // root, the OBJ converted into a temp folder is not, and its textures must
    // still be found.
    // ---------------------------------------------------------------------------

    // A real 4x4 RGB PNG (Nexus aborts on an invalid image).
    const std::vector<uint8_t> kTinyPng = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00,
        0x00, 0x26, 0x93, 0x09, 0x29, 0x00, 0x00, 0x00, 0x29, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0x0d, 0xc7, 0x31, 0x01, 0x00, 0x00, 0x0c, 0xc2, 0x30, 0x84, 0x55, 0x18, 0x67,
        0x45, 0x21, 0x70, 0xcb, 0x97, 0x24, 0x12, 0x1b, 0x17, 0x13, 0x04, 0x8b, 0xe3, 0x53,
        0xa9, 0xad, 0xeb, 0x67, 0x32, 0x3b, 0x37, 0x0f, 0xa7, 0x63, 0x13, 0x41, 0x06, 0x1a,
        0x2b, 0x55, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    // Geometry of a textured quad: 4 positions (48 B), 4 UVs (32 B), 6 indices (12 B).
    std::vector<uint8_t> texturedQuadBin() {
        const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        const std::vector<float> uv = {0, 0, 1, 0, 1, 1, 0, 1};
        const std::vector<uint16_t> idx = {0, 1, 2, 0, 2, 3};
        std::vector<uint8_t> bin = toBytes(pos);
        const auto uvBytes = toBytes(uv);
        const auto idxBytes = toBytes(idx);
        bin.insert(bin.end(), uvBytes.begin(), uvBytes.end());
        bin.insert(bin.end(), idxBytes.begin(), idxBytes.end());
        return bin;
    }

    // bufferField: the buffer object's extra fields (e.g. a uri). imageField: the image object.
    // extraBufferView: appended to bufferViews (for an embedded image), may be empty.
    std::string texturedQuadGltfJson(size_t bufferLength, const std::string& bufferUriField,
                                     const std::string& imageObject,
                                     const std::string& extraBufferView) {
        std::ostringstream j;
        j << "{\"asset\":{\"version\":\"2.0\"},"
          << "\"buffers\":[{\"byteLength\":" << bufferLength << bufferUriField << "}],"
          << "\"bufferViews\":["
             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
             "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":32},"
             "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":12}"
          << extraBufferView << "],"
          << "\"accessors\":["
             "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},"
             "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
             "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
          << "\"images\":[" << imageObject << "],"
          << "\"samplers\":[{}],"
             "\"textures\":[{\"source\":0,\"sampler\":0}],"
             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":0}]}],"
             "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
        return j.str();
    }

    struct GltfDataset : IndexFixture {
        explicit GltfDataset(const std::string& name) : IndexFixture(name, "") {}
        void add(const std::vector<std::string>& rels) {
            std::vector<std::string> abs;
            for (const auto& r : rels)
                abs.push_back((root / r).string());
            ddb::addToIndex(db.get(), abs);
        }
    };

    void writeGltfWithExternalTexture(const fs::path& gltf) {
        const auto bin = texturedQuadBin();
        fs::create_directories(gltf.parent_path());
        writeFile(gltf, texturedQuadGltfJson(bin.size(), ",\"uri\":\"" + dataUri(bin) + "\"",
                                                 "{\"uri\":\"tex.png\"}", ""));
    }

    TEST(file3d, buildGltfInSubfolderWithExternalTexture)
    {
        GltfDataset ds(TEST_NAME);
        writeGltfWithExternalTexture(ds.root / "sub" / "model.gltf");
        writeFile(ds.root / "sub" / "tex.png", kTinyPng);
        ds.add({"sub/model.gltf", "sub/tex.png"});

        Entry e;
        ASSERT_TRUE(ddb::getEntry(ds.db.get(), "sub/model.gltf", e));
        ASSERT_EQ(e.type, EntryType::Model);

        ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/model.gltf", "", false));
        EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/model.gltf"));
        EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
        EXPECT_TRUE(fs::exists(ds.db->buildDirectory() / e.hash / "nxs" / "model.nxz"));
    }

    // Builds rel and expects a plain AppException mentioning `mentions`, with no
    // ".pending" left behind (an unsatisfiable reference must not retry forever).
    void expectGltfBuildFailsWithoutPending(GltfDataset& ds, const std::string& rel,
                                            const std::string& mentions) {
        SCOPED_TRACE(rel);
        try {
            ddb::build(ds.db.get(), rel, "", false);
            FAIL() << "build of " << rel << " should fail";
        } catch (const BuildDepMissingException& e) {
            FAIL() << "an unsatisfiable glTF reference must not defer the build: " << e.what();
        } catch (const AppException& e) {
            EXPECT_NE(std::string(e.what()).find(mentions), std::string::npos) << e.what();
        }
        EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
        EXPECT_FALSE(fs::exists(ds.db->buildDirectory() / (indexedHash(ds.db.get(), rel) + ".pending")));
        EXPECT_FALSE(ddb::isBuildComplete(ds.db.get(), rel));
    }

    void writeGltfWithImageUri(const fs::path& gltf, const std::string& imageUri) {
        const auto bin = texturedQuadBin();
        fs::create_directories(gltf.parent_path());
        writeFile(gltf, texturedQuadGltfJson(bin.size(), ",\"uri\":\"" + dataUri(bin) + "\"",
                                                 "{\"uri\":\"" + imageUri + "\"}", ""));
    }

    // A glTF image that leaves the glTF's folder is never staged for the converted
    // model: even when the file exists in the dataset, the build fails outright.
    TEST(file3d, buildGltfImageOutsideItsFolderFailsWithoutPending)
    {
        GltfDataset ds(TEST_NAME);
        writeGltfWithImageUri(ds.root / "sub" / "model.gltf", "../shared/tex.png");
        writeFile(ds.root / "shared" / "tex.png", kTinyPng);
        ds.add({"sub/model.gltf", "shared/tex.png"});

        Entry e;
        ASSERT_TRUE(ddb::getEntry(ds.db.get(), "sub/model.gltf", e));
        ASSERT_EQ(e.type, EntryType::Model);
        expectGltfBuildFailsWithoutPending(ds, "sub/model.gltf", "../shared/tex.png");

        // buildPending has nothing to retry.
        ddb::buildPending(ds.db.get(), "", true);
        EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    }

    // Same for a buffer that leaves the glTF's folder, and for absolute paths/URIs.
    TEST(file3d, buildGltfUnsatisfiableUrisFailWithoutPending)
    {
        GltfDataset ds(TEST_NAME);
        const auto bin = texturedQuadBin();
        writeFile(ds.root / "shared" / "quad.bin", bin);
        writeFile(ds.root / "sub" / "tex.png", kTinyPng);
        writeFile(ds.root / "sub" / "buffer.gltf",
                      texturedQuadGltfJson(bin.size(), ",\"uri\":\"../shared/quad.bin\"",
                                           "{\"uri\":\"tex.png\"}", ""));
        writeGltfWithImageUri(ds.root / "sub" / "abs.gltf", "/abs/tex.png");
        writeGltfWithImageUri(ds.root / "sub" / "fileuri.gltf", "file:///abs/tex.png");
        ds.add({"sub/buffer.gltf", "sub/abs.gltf", "sub/fileuri.gltf", "sub/tex.png", "shared/quad.bin"});

        expectGltfBuildFailsWithoutPending(ds, "sub/buffer.gltf", "../shared/quad.bin");
        expectGltfBuildFailsWithoutPending(ds, "sub/abs.gltf", "/abs/tex.png");
        expectGltfBuildFailsWithoutPending(ds, "sub/fileuri.gltf", "file:///abs/tex.png");
    }

    // A remote (http/https) image is not a dependency and is never fetched. The
    // converted model still references it, which the temp-folder validation
    // reports (P3) as a plain AppException naming the original glTF: the build
    // fails and is never deferred with temp-folder names.
    TEST(file3d, buildGltfRemoteImageNeverPending)
    {
        GltfDataset ds(TEST_NAME);
        writeGltfWithImageUri(ds.root / "sub" / "model.gltf", "https://example.invalid/tex.png");
        ds.add({"sub/model.gltf"});

        EXPECT_TRUE(ddb::getGltfDependencies((ds.root / "sub" / "model.gltf").string()).empty());
        expectGltfBuildFailsWithoutPending(ds, "sub/model.gltf", "model.gltf references files that cannot be resolved");
    }

    TEST(file3d, buildGltfInSubfolderMissingTextureIsPending)
    {
        GltfDataset ds(TEST_NAME);
        writeGltfWithExternalTexture(ds.root / "sub" / "model.gltf");
        ds.add({"sub/model.gltf"});

        EXPECT_THROW(ddb::build(ds.db.get(), "sub/model.gltf", "", false), BuildDepMissingException);
        const auto pending = ddb::getPendingBuildInfo(ds.db.get());
        ASSERT_EQ(pending.size(), 1u);
        EXPECT_EQ(pending[0].missingDependencies, std::vector<std::string>{"sub/tex.png"});

        writeFile(ds.root / "sub" / "tex.png", kTinyPng);
        ds.add({"sub/tex.png"});
        ddb::buildPending(ds.db.get(), "", true);
        EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
        EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/model.gltf"));
    }

    TEST(file3d, buildGlbWithEmbeddedTextureInSubfolder)
    {
        GltfDataset ds(TEST_NAME);
        auto bin = texturedQuadBin();  // 92 bytes, 4-byte aligned
        const size_t imageOffset = bin.size();
        bin.insert(bin.end(), kTinyPng.begin(), kTinyPng.end());

        std::ostringstream view;
        view << ",{\"buffer\":0,\"byteOffset\":" << imageOffset << ",\"byteLength\":" << kTinyPng.size() << "}";
        const fs::path glb = ds.root / "sub" / "model.glb";
        fs::create_directories(glb.parent_path());
        writeGlbWithBin(glb,
                        texturedQuadGltfJson(bin.size(), "",
                                             "{\"bufferView\":3,\"mimeType\":\"image/png\"}",
                                             view.str()),
                        bin);
        ds.add({"sub/model.glb"});

        ASSERT_NO_THROW(ddb::build(ds.db.get(), "sub/model.glb", "", false));
        EXPECT_TRUE(ddb::isBuildComplete(ds.db.get(), "sub/model.glb"));
        EXPECT_FALSE(ddb::isBuildPending(ds.db.get()));
    }

}
