/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "3d.h"
#include "testfs.h"
#include "testarea.h"
#include <cpr/cpr.h>
#include <fstream>
#include <filesystem>
#include "exceptions.h"
#include <tiny_obj_loader.h>

namespace
{

    using namespace ddb;

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

}
