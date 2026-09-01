/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "3d.h"
#include "entry.h"
#include "test.h"
#include "testfs.h"
#include "testarea.h"
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

    void writeTextFile(const fs::path& path, const std::string& content) {
        std::ofstream out(path.string(), std::ios::binary);
        out << content;
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
            std::string archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/odm_texturing.zip";

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
            std::string archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/odm_texturing_leading_spaces.zip";

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
            std::string archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/odm_texturing_multiple.zip";

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
            std::string archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/metashape_obj.zip";

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
            std::string archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/metashape_obj_with_spaces.zip";

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
            std::string archiveUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/model-gltf.zip";

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
            std::string glbUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/model.glb";

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
            std::string glbUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/SunglassesKhronos.glb";

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
            std::string glbUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/IridescentDishWithOlives.glb";

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
            std::string glbUrl = "https://github.com/DroneDB/test_data/raw/refs/heads/master/3d/ToyCar.glb";

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
            std::string glbUrl = "https://raw.githubusercontent.com/DroneDB/test_data/refs/heads/master/3d/draco_model.glb";

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
        writeTextFile(gltf, triangleGltfJson(nullptr));
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Model);
    }

    TEST(file3d, identifyGltfTriangleStripAndFanAreModel)
    {
        TestArea ta(TEST_NAME);
        const fs::path strip = ta.getPath("strip.gltf");
        writeTextFile(strip, triangleGltfJson("5"));
        EXPECT_EQ(ddb::fingerprint(strip), EntryType::Model);

        const fs::path fan = ta.getPath("fan.gltf");
        writeTextFile(fan, triangleGltfJson("6"));
        EXPECT_EQ(ddb::fingerprint(fan), EntryType::Model);
    }

    TEST(file3d, identifyGltfPointModeIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("points.gltf");
        writeTextFile(gltf, triangleGltfJson("0"));
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Generic);
    }

    TEST(file3d, identifyGltfLineModesAreGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path lines = ta.getPath("lines.gltf");
        writeTextFile(lines, triangleGltfJson("1"));
        EXPECT_EQ(ddb::fingerprint(lines), EntryType::Generic);
    }

    TEST(file3d, identifyGltfMalformedJsonIsGenericAndDoesNotThrow)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("garbage.gltf");
        writeTextFile(gltf, "{ this is not valid json ");
        EntryType type = EntryType::Undefined;
        ASSERT_NO_THROW(type = ddb::fingerprint(gltf));
        EXPECT_EQ(type, EntryType::Generic);
    }

    TEST(file3d, identifyGltfEmptyMeshesArrayIsGeneric)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("nomeshes.gltf");
        writeTextFile(gltf, "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[]}");
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Generic);
    }

    // glTF 1.0 declares "meshes" as an object keyed by mesh id rather than an array.
    TEST(file3d, identifyGltf1DictMeshesIsModel)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("gltf1.gltf");
        writeTextFile(gltf,
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

    // Regression test: a mesh mixing a triangle primitive with a point primitive must not
    // reach libnexus' triangle-soup PLY reader with desynchronized 1-index face records.
    // Without AI_CONFIG_PP_SBP_REMOVE in convertGltfTo3dModel, this used to throw
    // "Bad index in triangle list" (or worse) deep inside nexusBuild.
    TEST(file3d, buildNexusMixedTrianglePointDoesNotCrash)
    {
        TestArea ta(TEST_NAME);
        const fs::path gltf = ta.getPath("mixed.gltf");
        writeTextFile(gltf, mixedTrianglePointGltfJson());

        // The classifier only requires one triangle primitive to declare the asset a Model.
        EXPECT_EQ(ddb::fingerprint(gltf), EntryType::Model);

        const fs::path nxz = ta.getPath("mixed.nxz");
        std::string outNxz;
        ASSERT_NO_THROW(outNxz = buildNexus(gltf.string(), nxz.string(), true));
        ASSERT_TRUE(fs::exists(outNxz));
        EXPECT_GT(fs::file_size(outNxz), 0);
    }

}
