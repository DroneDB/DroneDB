/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "3d.h"
#include "testfs.h"

namespace
{

    using namespace ddb;

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

}
