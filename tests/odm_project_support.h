/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ODM_PROJECT_SUPPORT_H
#define ODM_PROJECT_SUPPORT_H

// A small ODM project on disk for the tests of the training-data export: cameras.json,
// odm_report/shots.geojson, images/, odm_georeferencing/coords.txt and a point cloud, the way
// OpenDroneMap writes them (the layout and the conventions of the files are those of a real
// project: normalized camera parameters, absolute camera centres, axis-angle rotations from the
// world to the camera).

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fs.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "json.h"
#include "mio.h"

namespace testutils
{
    struct OdmCameraSpec
    {
        std::string id;
        json params; // what cameras.json holds for the camera
    };

    struct OdmShotSpec
    {
        std::string filename;
        std::string camera;                  // the "camera" property of the shot; empty for none
        std::array<double, 3> centre{};      // absolute, in the CRS of the project
        std::array<double, 3> rotation{};    // axis-angle, world to camera
        int width = 640;
        int height = 480;
        double focal = 0.8;
    };

    struct OdmPointSpec
    {
        double x = 0, y = 0, z = 0; // absolute, in the CRS of the project
        uint8_t red = 0, green = 0, blue = 0;
    };

    enum class OdmCloud
    {
        None,
        Ply, // odm_filterpoints/point_cloud.ply, shifted by the offset
        Laz  // odm_georeferencing/odm_georeferenced_model.laz: written by the test that needs it
    };

    struct OdmProjectSpec
    {
        std::string crsLine = "WGS84 UTM 15N";
        double offsetEasting = 576705.0;
        double offsetNorthing = 5188170.0;
        bool writeCoords = true;
        std::vector<OdmCameraSpec> cameras;
        std::vector<OdmShotSpec> shots;
        std::vector<OdmPointSpec> points;
        OdmCloud cloud = OdmCloud::Ply;
    };

    // The Brown camera of the tests: 640 x 480, f = 0.8 of the longest side
    inline OdmCameraSpec brownCamera(const std::string &id = "test cam 640 480 brown 0.8", double k3 = 0.0)
    {
        return {id,
                {{"projection_type", "brown"},
                 {"width", 640},
                 {"height", 480},
                 {"focal_x", 0.8},
                 {"focal_y", 0.8},
                 {"c_x", 0.01},
                 {"c_y", -0.02},
                 {"k1", -0.05},
                 {"k2", 0.01},
                 {"p1", 0.001},
                 {"p2", -0.0005},
                 {"k3", k3}}};
    }

    // Two shots looking forward and down, a few meters apart, and four points in front of them
    inline OdmProjectSpec defaultOdmProject()
    {
        OdmProjectSpec spec;
        spec.cameras.push_back(brownCamera());
        OdmShotSpec a;
        a.filename = "a.JPG";
        a.camera = "v2 test cam 640 480 brown 0.8";
        a.centre = {spec.offsetEasting + 10.0, spec.offsetNorthing + 20.0, 120.0};
        a.rotation = {1.9, 0.1, -0.05};
        OdmShotSpec b = a;
        b.filename = "b.JPG";
        b.centre = {spec.offsetEasting + 14.0, spec.offsetNorthing + 21.0, 121.0};
        b.rotation = {2.2, -0.2, 0.3};
        spec.shots = {a, b};
        spec.points = {{spec.offsetEasting + 5.5, spec.offsetNorthing + 12.25, 101.5, 10, 20, 30},
                       {spec.offsetEasting + 6.5, spec.offsetNorthing + 13.25, 102.5, 40, 50, 60},
                       {spec.offsetEasting - 7.5, spec.offsetNorthing + 2.75, 99.5, 70, 80, 90},
                       {spec.offsetEasting + 20.0, spec.offsetNorthing + 30.0, 100.0, 255, 128, 0}};
        return spec;
    }

    // A JPEG of one flat colour
    inline void writeJpeg(const fs::path &file, int width, int height, uint8_t value = 90)
    {
        GDALDriverH mem = GDALGetDriverByName("MEM");
        auto ds = ddb::adoptDataset(GDALCreate(mem, "", width, height, 3, GDT_Byte, nullptr));
        std::vector<uint8_t> row(static_cast<size_t>(width) * height, value);
        for (int b = 1; b <= 3; ++b)
            if (GDALRasterIO(GDALGetRasterBand(ddb::datasetHandle(ds), b), GF_Write, 0, 0, width, height, row.data(), width,
                             height, GDT_Byte, 0, 0) != CE_None)
                throw std::runtime_error("Cannot fill the test image");
        auto jpeg = ddb::adoptDataset(GDALCreateCopy(GDALGetDriverByName("JPEG"), ddb::io::toUtf8(file).c_str(),
                                                     ddb::datasetHandle(ds), FALSE, nullptr, nullptr, nullptr));
        if (!jpeg)
            throw std::runtime_error("Cannot write the test image " + ddb::io::toUtf8(file));
        jpeg.reset();
    }

    // A binary little-endian PLY the way ODM's filter writes it: float coordinates and normals, the
    // colours in the order red, blue, green, and a trailing property
    inline void writeOdmPly(const fs::path &file, const std::vector<OdmPointSpec> &points, double shiftX, double shiftY)
    {
        std::ofstream out(file, std::ios::binary);
        out << "ply\nformat binary_little_endian 1.0\ncomment Generated by FPCFilter v0.1\nelement vertex "
            << points.size()
            << "\nproperty float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\n"
               "property float nz\nproperty uchar red\nproperty uchar blue\nproperty uchar green\n"
               "property uchar views\nend_header\n";
        for (const auto &p : points)
        {
            const float v[6] = {static_cast<float>(p.x - shiftX), static_cast<float>(p.y - shiftY),
                                static_cast<float>(p.z), 0.f, 0.f, 1.f};
            out.write(reinterpret_cast<const char *>(v), sizeof v);
            const uint8_t c[4] = {p.red, p.blue, p.green, 3};
            out.write(reinterpret_cast<const char *>(c), sizeof c);
        }
    }

    inline void writeTextFile(const fs::path &file, const std::string &content)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << content;
    }

    // Writes the project into folder (which is created) and returns it
    inline fs::path writeOdmProject(const fs::path &folder, const OdmProjectSpec &spec)
    {
        fs::create_directories(folder / "images");
        fs::create_directories(folder / "odm_report");
        fs::create_directories(folder / "odm_georeferencing");

        json cameras = json::object();
        for (const auto &c : spec.cameras)
            cameras[c.id] = c.params;
        writeTextFile(folder / "cameras.json", cameras.dump(4));

        json features = json::array();
        for (const auto &s : spec.shots)
        {
            json properties = {{"filename", s.filename},
                                    {"focal", s.focal},
                                    {"width", s.width},
                                    {"height", s.height},
                                    {"translation", {s.centre[0], s.centre[1], s.centre[2]}},
                                    {"rotation", {s.rotation[0], s.rotation[1], s.rotation[2]}}};
            if (!s.camera.empty())
                properties["camera"] = s.camera;
            features.push_back({{"type", "Feature"},
                                {"properties", properties},
                                {"geometry", {{"type", "Point"}, {"coordinates", {-91.99, 46.84, s.centre[2]}}}}});
            writeJpeg(folder / "images" / ddb::io::fromUtf8(s.filename), s.width, s.height);
        }
        writeTextFile(folder / "odm_report" / "shots.geojson",
                      json{{"type", "FeatureCollection"}, {"features", features}}.dump(2));

        if (spec.writeCoords)
            writeTextFile(folder / "odm_georeferencing" / "coords.txt",
                          spec.crsLine + "\n" + std::to_string(static_cast<long long>(spec.offsetEasting)) + " " +
                              std::to_string(static_cast<long long>(spec.offsetNorthing)) + "\n-41.9 -5.4 198.3\n");

        if (spec.cloud == OdmCloud::Ply)
        {
            fs::create_directories(folder / "odm_filterpoints");
            writeOdmPly(folder / "odm_filterpoints" / "point_cloud.ply", spec.points, spec.offsetEasting,
                        spec.offsetNorthing);
        }
        return folder;
    }
} // namespace testutils

#endif // ODM_PROJECT_SUPPORT_H
