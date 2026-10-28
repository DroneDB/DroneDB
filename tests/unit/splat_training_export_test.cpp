/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The training data of a Gaussian splat made from an ODM project (exportSplatTrainingData). The
// COLMAP files it writes are read back here by a reader of the format that shares nothing with the
// writer, and a point is projected both the way ODM defines it (the Brown model of OpenSfM, the
// rotation as the exponential of an axis-angle matrix) and the way COLMAP defines OPENCV, to see
// that the two agree on the pixel.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "exceptions.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "json.h"
#include "local_frame_georef.h"
#include "mio.h"
#include "odm_project_support.h"
#include "splat_training_export.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;
    using namespace testutils;
    using Vec3 = std::array<double, 3>;

    // ---- A reader of COLMAP's binary model, from the format description ----

    struct Reader
    {
        std::vector<uint8_t> bytes;
        size_t pos = 0;

        explicit Reader(const fs::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            EXPECT_TRUE(in.good()) << file;
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        template <typename T>
        T get()
        {
            EXPECT_LE(pos + sizeof(T), bytes.size());
            T v{};
            std::memcpy(&v, bytes.data() + pos, sizeof(T));
            pos += sizeof(T);
            return v;
        }
        std::string cString()
        {
            std::string s;
            while (pos < bytes.size() && bytes[pos] != 0)
                s.push_back(static_cast<char>(bytes[pos++]));
            ++pos;
            return s;
        }
        bool atEnd() const { return pos == bytes.size(); }
    };

    struct Camera
    {
        uint32_t id = 0;
        int32_t model = 0;
        uint64_t width = 0, height = 0;
        std::vector<double> params;
    };
    struct Image
    {
        uint32_t id = 0;
        std::array<double, 4> q{}; // w, x, y, z
        Vec3 t{};
        uint32_t camera = 0;
        std::string name;
        uint64_t points2d = 0;
    };
    struct Point
    {
        uint64_t id = 0;
        Vec3 xyz{};
        std::array<uint8_t, 3> rgb{};
        double error = 0;
        uint64_t track = 0;
    };

    // The number of parameters of the models, from COLMAP's list of camera models
    size_t paramsOfModel(int32_t model)
    {
        switch (model)
        {
        case 0: return 3; // SIMPLE_PINHOLE
        case 1: return 4; // PINHOLE
        case 2: return 4; // SIMPLE_RADIAL
        case 3: return 5; // RADIAL
        case 4: return 8; // OPENCV
        case 5: return 8; // OPENCV_FISHEYE
        case 6: return 12; // FULL_OPENCV
        default: ADD_FAILURE() << "model " << model; return 0;
        }
    }

    std::vector<Camera> readCameras(const fs::path &file)
    {
        Reader r(file);
        std::vector<Camera> cameras(r.get<uint64_t>());
        for (auto &c : cameras)
        {
            c.id = r.get<uint32_t>();
            c.model = r.get<int32_t>();
            c.width = r.get<uint64_t>();
            c.height = r.get<uint64_t>();
            for (size_t i = 0; i < paramsOfModel(c.model); ++i)
                c.params.push_back(r.get<double>());
        }
        EXPECT_TRUE(r.atEnd()) << "bytes left in cameras.bin";
        return cameras;
    }

    std::vector<Image> readImages(const fs::path &file)
    {
        Reader r(file);
        std::vector<Image> images(r.get<uint64_t>());
        for (auto &i : images)
        {
            i.id = r.get<uint32_t>();
            for (auto &q : i.q)
                q = r.get<double>();
            for (auto &t : i.t)
                t = r.get<double>();
            i.camera = r.get<uint32_t>();
            i.name = r.cString();
            i.points2d = r.get<uint64_t>();
            for (uint64_t k = 0; k < i.points2d; ++k)
            {
                r.get<double>();
                r.get<double>();
                r.get<int64_t>();
            }
        }
        EXPECT_TRUE(r.atEnd()) << "bytes left in images.bin";
        return images;
    }

    std::vector<Point> readPoints(const fs::path &file)
    {
        Reader r(file);
        std::vector<Point> points(r.get<uint64_t>());
        for (auto &p : points)
        {
            p.id = r.get<uint64_t>();
            for (auto &c : p.xyz)
                c = r.get<double>();
            for (auto &c : p.rgb)
                c = r.get<uint8_t>();
            p.error = r.get<double>();
            p.track = r.get<uint64_t>();
            for (uint64_t k = 0; k < p.track; ++k)
            {
                r.get<uint32_t>();
                r.get<uint32_t>();
            }
        }
        EXPECT_TRUE(r.atEnd()) << "bytes left in points3D.bin";
        return points;
    }

    // ---- The two projections ----

    // exp([w]x) by its series: the rotation of an axis-angle vector, with nothing in common with the
    // closed form of the writer
    std::array<Vec3, 3> expSkew(const Vec3 &w)
    {
        const std::array<Vec3, 3> K = {{{0, -w[2], w[1]}, {w[2], 0, -w[0]}, {-w[1], w[0], 0}}};
        std::array<Vec3, 3> sum = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        std::array<Vec3, 3> term = sum;
        for (int n = 1; n < 40; ++n)
        {
            std::array<Vec3, 3> next{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k)
                        next[i][j] += term[i][k] * K[k][j] / n;
            term = next;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    sum[i][j] += term[i][j];
        }
        return sum;
    }

    Vec3 mul(const std::array<Vec3, 3> &m, const Vec3 &v)
    {
        return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
    }

    Vec3 mulTransposed(const std::array<Vec3, 3> &m, const Vec3 &v)
    {
        return {m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2], m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2],
                m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2]};
    }

    // The pixel of a point in the camera frame in the Brown model of OpenSfM: the image is
    // normalized by its longest side
    std::array<double, 2> odmPixel(const json &camera, const Vec3 &pc)
    {
        const double w = camera["width"], h = camera["height"], side = std::max(w, h);
        const double x = pc[0] / pc[2], y = pc[1] / pc[2];
        const double r2 = x * x + y * y;
        const double radial = 1 + r2 * (camera.value("k1", 0.0) + r2 * (camera.value("k2", 0.0) + r2 * camera.value("k3", 0.0)));
        const double p1 = camera.value("p1", 0.0), p2 = camera.value("p2", 0.0);
        const double xd = x * radial + 2 * p1 * x * y + p2 * (r2 + 2 * x * x);
        const double yd = y * radial + p1 * (r2 + 2 * y * y) + 2 * p2 * x * y;
        return {w / 2 + side * (camera["focal_x"].get<double>() * xd + camera.value("c_x", 0.0)),
                h / 2 + side * (camera["focal_y"].get<double>() * yd + camera.value("c_y", 0.0))};
    }

    // The pixel of a point of the model in COLMAP's OPENCV model; the third element is the depth
    std::array<double, 3> colmapPixel(const Camera &camera, const Image &image, const Vec3 &p)
    {
        EXPECT_EQ(camera.model, 4);
        const double w = image.q[0], x = image.q[1], y = image.q[2], z = image.q[3];
        const std::array<Vec3, 3> R = {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                                        {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                                        {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
        Vec3 pc = mul(R, p);
        for (int i = 0; i < 3; ++i)
            pc[i] += image.t[i];
        const auto &k = camera.params; // fx fy cx cy k1 k2 p1 p2
        const double u = pc[0] / pc[2], v = pc[1] / pc[2], r2 = u * u + v * v;
        const double radial = k[4] * r2 + k[5] * r2 * r2;
        const double du = u * radial + 2 * k[6] * u * v + k[7] * (r2 + 2 * u * u);
        const double dv = v * radial + k[6] * (r2 + 2 * v * v) + 2 * k[7] * u * v;
        return {k[0] * (u + du) + k[2], k[1] * (v + dv) + k[3], pc[2]};
    }

    // ---- The fixtures ----

    struct Exported
    {
        TestArea area;
        fs::path project;
        fs::path out;
        OdmProjectSpec spec;
        SplatTrainingExportResult result;

        explicit Exported(const std::string &name)
            : area(name, true), project(area.getFolder("odm")), out(area.getPath("out")), spec(defaultOdmProject())
        {
        }

        void write() { writeOdmProject(project, spec); }
        SplatTrainingExportResult run(const SplatTrainingExportOptions &options = {})
        {
            result = exportSplatTrainingData(project, out, options);
            return result;
        }
        fs::path sparse(const char *file) const { return out / "sparse" / "0" / file; }
    };

    bool hasWarning(const SplatTrainingExportResult &r, const std::string &text)
    {
        for (const auto &w : r.warnings)
            if (w.find(text) != std::string::npos)
                return true;
        return false;
    }

    std::string allWarnings(const SplatTrainingExportResult &r)
    {
        std::string s;
        for (const auto &w : r.warnings)
            s += w + "\n";
        return s;
    }

    // Where a point at pc (camera frame) of a shot is in the world (absolute)
    Vec3 worldPoint(const OdmShotSpec &shot, const Vec3 &pc)
    {
        const Vec3 d = mulTransposed(expSkew(shot.rotation), pc);
        return {shot.centre[0] + d[0], shot.centre[1] + d[1], shot.centre[2] + d[2]};
    }

    template <typename T>
    void put(std::ofstream &out, T value)
    {
        out.write(reinterpret_cast<const char *>(&value), sizeof value);
    }

    // A LAS 1.2 file (point format 2, uncompressed: PDAL reads it by the header, whatever the
    // extension) with the points in absolute coordinates; the colours are 16-bit
    void writeLas(const fs::path &file, const std::vector<OdmPointSpec> &points)
    {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        const auto zeros = [&](size_t n) {
            const std::string z(n, '\0');
            out.write(z.data(), static_cast<std::streamsize>(n));
        };
        const double scale = 0.001;
        const double offset[3] = {576000.0, 5188000.0, 0.0};
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (const auto &p : points)
        {
            const double c[3] = {p.x, p.y, p.z};
            for (int i = 0; i < 3; ++i)
            {
                lo[i] = std::min(lo[i], c[i]);
                hi[i] = std::max(hi[i], c[i]);
            }
        }
        out.write("LASF", 4);
        zeros(2 + 2 + 4 + 2 + 2 + 8);  // source id, encoding, GUID
        put(out, static_cast<uint8_t>(1));
        put(out, static_cast<uint8_t>(2));  // version 1.2
        zeros(32 + 32);  // system and software
        put(out, static_cast<uint16_t>(1));
        put(out, static_cast<uint16_t>(2026));  // day and year
        put(out, static_cast<uint16_t>(227));  // header size
        put(out, static_cast<uint32_t>(227));  // offset to the points
        put(out, static_cast<uint32_t>(0));  // VLRs
        put(out, static_cast<uint8_t>(2));  // point format
        put(out, static_cast<uint16_t>(26));  // record length
        put(out, static_cast<uint32_t>(points.size()));
        zeros(5 * 4);  // points by return
        for (int i = 0; i < 3; ++i)
            put(out, scale);
        for (double o : offset)
            put(out, o);
        put(out, hi[0]);
        put(out, lo[0]);
        put(out, hi[1]);
        put(out, lo[1]);
        put(out, hi[2]);
        put(out, lo[2]);
        for (const auto &p : points)
        {
            put(out, static_cast<int32_t>(std::lround((p.x - offset[0]) / scale)));
            put(out, static_cast<int32_t>(std::lround((p.y - offset[1]) / scale)));
            put(out, static_cast<int32_t>(std::lround((p.z - offset[2]) / scale)));
            zeros(2 + 1 + 1 + 1 + 1 + 2);          // intensity, flags, class, angle, user data, source
            put(out, static_cast<uint16_t>(p.red * 257));
            put(out, static_cast<uint16_t>(p.green * 257));
            put(out, static_cast<uint16_t>(p.blue * 257));
        }
    }


    // The size of an image file
    std::array<int, 2> sizeOfImage(const fs::path &file)
    {
        const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
        EXPECT_TRUE(ds) << file;
        if (!ds)
            return {0, 0};
        return {GDALGetRasterXSize(datasetHandle(ds)), GDALGetRasterYSize(datasetHandle(ds))};
    }

    // ---- The conventions ----

    // The camera centre, the rotation and the intrinsics of ODM give the pixel COLMAP gives from the
    // model written: the same projection from two independent definitions
    TEST(SplatTrainingExport, projectsAPointToTheSamePixelAsOdm)
    {
        Exported e(TEST_NAME);
        e.write();
        const auto result = e.run();
        ASSERT_EQ(result.images, 2u);

        const auto cameras = readCameras(e.sparse("cameras.bin"));
        const auto images = readImages(e.sparse("images.bin"));
        ASSERT_EQ(cameras.size(), 1u);
        ASSERT_EQ(images.size(), 2u);
        const Vec3 offset = {e.spec.offsetEasting, e.spec.offsetNorthing, 0.0};

        // Points seen by the middle and the corners of the image, near and far
        const std::vector<Vec3> inCamera = {{0, 0, 10}, {3.1, -2.2, 10}, {-4.9, 3.6, 8}, {0.4, 0.3, 60}, {-2, -1, 3}};
        for (size_t s = 0; s < images.size(); ++s)
        {
            const auto &shot = e.spec.shots[s];
            for (const auto &pc : inCamera)
            {
                SCOPED_TRACE(shot.filename + " " + std::to_string(pc[0]) + "," + std::to_string(pc[1]) + "," +
                             std::to_string(pc[2]));
                const Vec3 world = worldPoint(shot, pc);
                const auto expected = odmPixel(e.spec.cameras[0].params, pc);

                // The model is in the CRS minus the offset
                const Vec3 local = {world[0] - offset[0], world[1] - offset[1], world[2] - offset[2]};
                const auto actual = colmapPixel(cameras[0], images[s], local);
                EXPECT_NEAR(actual[0], expected[0], 1e-6);
                EXPECT_NEAR(actual[1], expected[1], 1e-6);
                EXPECT_NEAR(actual[2], pc[2], 1e-9) << "the depth is the one of the camera frame";
            }
        }
    }

    TEST(SplatTrainingExport, writesCamerasImagesAndPointsThatReadBack)
    {
        Exported e(TEST_NAME);
        e.write();
        const auto result = e.run();
        EXPECT_EQ(result.images, 2u);
        EXPECT_EQ(result.points, 4u);
        EXPECT_TRUE(result.warnings.empty()) << allWarnings(result);

        const auto cameras = readCameras(e.sparse("cameras.bin"));
        ASSERT_EQ(cameras.size(), 1u);
        EXPECT_EQ(cameras[0].id, 1u);
        EXPECT_EQ(cameras[0].model, 4) << "OPENCV";
        EXPECT_EQ(cameras[0].width, 640u);
        EXPECT_EQ(cameras[0].height, 480u);
        const auto &odm = e.spec.cameras[0].params;
        const std::vector<double> expected = {0.8 * 640, 0.8 * 640, 320 + 0.01 * 640, 240 - 0.02 * 640,
                                              odm["k1"], odm["k2"], odm["p1"], odm["p2"]};
        ASSERT_EQ(cameras[0].params.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            EXPECT_DOUBLE_EQ(cameras[0].params[i], expected[i]) << i;

        const auto images = readImages(e.sparse("images.bin"));
        ASSERT_EQ(images.size(), 2u);
        EXPECT_EQ(images[0].id, 1u);
        EXPECT_EQ(images[1].id, 2u);
        EXPECT_EQ(images[0].name, "a.JPG");
        EXPECT_EQ(images[1].name, "b.JPG");
        for (const auto &i : images)
        {
            EXPECT_EQ(i.camera, 1u);
            EXPECT_EQ(i.points2d, 0u);
            const double norm = std::sqrt(i.q[0] * i.q[0] + i.q[1] * i.q[1] + i.q[2] * i.q[2] + i.q[3] * i.q[3]);
            EXPECT_NEAR(norm, 1.0, 1e-12);
            EXPECT_GE(i.q[0], 0.0);
            EXPECT_TRUE(fs::is_regular_file(e.out / "images" / io::fromUtf8(i.name)));
        }

        const auto points = readPoints(e.sparse("points3D.bin"));
        ASSERT_EQ(points.size(), 4u);
        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto &want = e.spec.points[i];
            EXPECT_EQ(points[i].id, i + 1);
            EXPECT_NEAR(points[i].xyz[0], want.x - e.spec.offsetEasting, 1e-4);
            EXPECT_NEAR(points[i].xyz[1], want.y - e.spec.offsetNorthing, 1e-4);
            EXPECT_NEAR(points[i].xyz[2], want.z, 1e-4);
            // The PLY lists red, blue, green: the colours follow the names
            EXPECT_EQ(points[i].rgb[0], want.red);
            EXPECT_EQ(points[i].rgb[1], want.green);
            EXPECT_EQ(points[i].rgb[2], want.blue);
            EXPECT_EQ(points[i].track, 0u);
        }
    }

    TEST(SplatTrainingExport, readsTheGeoreferencedLasInsteadOfThePly)
    {
        Exported e(TEST_NAME);
        e.spec.cloud = OdmCloud::None;
        e.write();
        // The LAS is in the CRS of the project, the model is that minus the offset; a PLY with other
        // points proves the LAS is the one read
        writeOdmPly(e.project / "odm_filterpoints" / "point_cloud.ply", {{1, 2, 3, 1, 1, 1}}, 0, 0);
        writeLas(e.project / "odm_georeferencing" / "odm_georeferenced_model.laz", e.spec.points);

        const auto result = e.run();
        EXPECT_EQ(result.points, 4u);
        const auto points = readPoints(e.sparse("points3D.bin"));
        ASSERT_EQ(points.size(), 4u);
        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto &want = e.spec.points[i];
            EXPECT_NEAR(points[i].xyz[0], want.x - e.spec.offsetEasting, 2e-3);
            EXPECT_NEAR(points[i].xyz[1], want.y - e.spec.offsetNorthing, 2e-3);
            EXPECT_NEAR(points[i].xyz[2], want.z, 2e-3);
            // 16-bit colours of the LAS
            EXPECT_EQ(points[i].rgb[0], want.red);
            EXPECT_EQ(points[i].rgb[1], want.green);
            EXPECT_EQ(points[i].rgb[2], want.blue);
        }
    }

    TEST(SplatTrainingExport, thinsTheCloudOnAGridToTheMostPointsAsked)
    {
        Exported e(TEST_NAME);
        // 40 x 40 x 5 points a meter apart, 3 cm of jitter
        e.spec.points.clear();
        for (int i = 0; i < 40; ++i)
            for (int j = 0; j < 40; ++j)
                for (int k = 0; k < 5; ++k)
                    e.spec.points.push_back({e.spec.offsetEasting + i + 0.03 * ((j * 7) % 3), e.spec.offsetNorthing + j,
                                             100.0 + 0.5 * k, static_cast<uint8_t>(i), static_cast<uint8_t>(j), 7});
        e.write();

        SplatTrainingExportOptions options;
        options.maxPoints = 500;
        const auto result = e.run(options);
        EXPECT_LE(result.points, 500u);
        EXPECT_GE(result.points, 60u) << "the grid keeps a share of the limit, not a handful of points";

        const auto points = readPoints(e.sparse("points3D.bin"));
        EXPECT_EQ(points.size(), result.points);
        // What stays is spread over the whole cloud, not the first points of the file
        double maxX = 0, maxY = 0;
        for (const auto &p : points)
        {
            maxX = std::max(maxX, p.xyz[0]);
            maxY = std::max(maxY, p.xyz[1]);
        }
        EXPECT_GT(maxX, 30.0);
        EXPECT_GT(maxY, 30.0);

        // A limit that is more than the cloud keeps it whole
        options.maxPoints = 100000;
        EXPECT_EQ(e.run(options).points, e.spec.points.size());
    }

    TEST(SplatTrainingExport, warnsOfAnEmptyOrMissingCloud)
    {
        {
            Exported e(TEST_NAME + std::string("-none"));
                e.spec.cloud = OdmCloud::None;
            e.write();
            const auto result = e.run();
            EXPECT_EQ(result.points, 0u);
            EXPECT_TRUE(hasWarning(result, "no point cloud")) << allWarnings(result);
            EXPECT_TRUE(readPoints(e.sparse("points3D.bin")).empty());
            EXPECT_EQ(readImages(e.sparse("images.bin")).size(), 2u);
        }
        {
            Exported e(TEST_NAME + std::string("-zero"));
            e.write();
            SplatTrainingExportOptions options;
            options.maxPoints = 0;
            const auto result = e.run(options);
            EXPECT_EQ(result.points, 0u);
            EXPECT_TRUE(hasWarning(result, "points3D.bin is empty")) << allWarnings(result);
        }
        {
            Exported e(TEST_NAME + std::string("-empty"));
                e.spec.points.clear();
            e.write();
            const auto result = e.run();
            EXPECT_TRUE(hasWarning(result, "has no point")) << allWarnings(result);
        }
        {
            // A cloud somewhere else is not the cloud of these cameras
            Exported e(TEST_NAME + std::string("-away"));
                for (auto &p : e.spec.points)
                p.x += 50000.0;
            e.write();
            const auto result = e.run();
            EXPECT_TRUE(hasWarning(result, "lies away from the cameras")) << allWarnings(result);
        }
    }

    // ---- The georeference ----

    TEST(SplatTrainingExport, writesTheGeorefOfASplatTrainedFromIt)
    {
        Exported e(TEST_NAME);
        e.write();
        e.run();

        std::ifstream sidecarFile(e.out / "georef.json");
        const auto sidecar = json::parse(sidecarFile);
        EXPECT_EQ(sidecar["crs"], "EPSG:32615");
        EXPECT_EQ(sidecar["offset"], (json{576705.0, 5188170.0, 0.0}));

        // Put next to a splat, it is its georeference
        const auto georef = detectLocalFrameGeoref(io::toUtf8(e.out / "splat.ply"));
        ASSERT_TRUE(georef.has_value());
        EXPECT_EQ(georef->kind, LocalFrameGeoref::Kind::Projected);
        EXPECT_EQ(georef->crs, "EPSG:32615");
        EXPECT_DOUBLE_EQ(georef->offset[0], 576705.0);
        EXPECT_DOUBLE_EQ(georef->offset[1], 5188170.0);
        EXPECT_DOUBLE_EQ(georef->offset[2], 0.0);
    }

    TEST(SplatTrainingExport, keepsACrsGivenAsAProjString)
    {
        Exported e(TEST_NAME);
        e.spec.crsLine = "+proj=utm +zone=15 +datum=WGS84 +units=m +no_defs";
        e.write();
        e.run();

        const auto georef = detectLocalFrameGeoref(io::toUtf8(e.out / "splat.spz"));
        ASSERT_TRUE(georef.has_value());
        EXPECT_EQ(georef->crs, e.spec.crsLine);
        EXPECT_DOUBLE_EQ(georef->offset[0], 576705.0);
    }

    // The CRS and the offset of the export are those of coords.txt, whatever sidecar sits beside it: a
    // georef.json there (an earlier export put into the project, a hand-made one) is read by the
    // search for the georeference of a file, which comes first to it
    TEST(SplatTrainingExport, takesTheGeorefOfCoordsTxtNotOfASiblingGeorefJson)
    {
        Exported e(TEST_NAME);
        e.write();
        const fs::path georefFolder = e.project / "odm_georeferencing";
        const fs::path sibling = georefFolder / "georef.json";
        {
            std::ofstream out(sibling);
            out << R"({"crs": "EPSG:32633", "offset": [1.0, 2.0, 3.0]})";
        }

        // The premise: the search of a file in that folder does find the sibling first
        const auto searched = detectLocalFrameGeoref(io::toUtf8(georefFolder / "coords.txt"));
        ASSERT_TRUE(searched.has_value());
        ASSERT_EQ(searched->crs, "EPSG:32633");

        const auto coords = parseOdmCoords(io::toUtf8(georefFolder / "coords.txt"));
        ASSERT_TRUE(coords.has_value());
        EXPECT_EQ(coords->crs, "EPSG:32615");

        e.run();
        std::ifstream sidecarFile(e.out / "georef.json");
        const auto sidecar = json::parse(sidecarFile);
        EXPECT_EQ(sidecar["crs"], "EPSG:32615");
        EXPECT_EQ(sidecar["offset"], (json{576705.0, 5188170.0, 0.0}));
    }

    TEST(SplatTrainingExport, aFileThatIsNotCoordsTxtIsNoOdmGeoref)
    {
        TestArea area(TEST_NAME, true);
        EXPECT_FALSE(parseOdmCoords(io::toUtf8(area.getPath("missing.txt"))).has_value());

        const fs::path oneLine = area.getPath("one-line.txt");
        {
            std::ofstream out(oneLine);
            out << "WGS84 UTM 15N\n";
        }
        EXPECT_FALSE(parseOdmCoords(io::toUtf8(oneLine)).has_value());

        // A georef.json is read by the search for a georeference, not as an ODM file
        const fs::path sidecarJson = area.getPath("georef.json");
        {
            std::ofstream out(sidecarJson);
            out << R"({"crs": "EPSG:32615", "offset": [1, 2]})";
        }
        EXPECT_FALSE(parseOdmCoords(io::toUtf8(sidecarJson)).has_value());
        EXPECT_TRUE(detectLocalFrameGeoref(io::toUtf8(sidecarJson)).has_value());
    }

    // ---- The images ----

    TEST(SplatTrainingExport, linksTheOriginalImagesWhenNoSizeIsAsked)
    {
        Exported e(TEST_NAME);
        e.write();
        e.run();
        for (const char *name : {"a.JPG", "b.JPG"})
        {
            EXPECT_EQ(fs::file_size(e.out / "images" / name), fs::file_size(e.project / "images" / name));
            EXPECT_EQ(sizeOfImage(e.out / "images" / name), (std::array<int, 2>{640, 480}));
        }
    }

    TEST(SplatTrainingExport, resizesTheImagesAndTheirCamerasKeepingTheAspect)
    {
        Exported e(TEST_NAME);
        e.write();
        SplatTrainingExportOptions options;
        options.maxImageSize = 64;
        const auto result = e.run(options);
        EXPECT_TRUE(result.warnings.empty()) << allWarnings(result);

        for (const char *name : {"a.JPG", "b.JPG"})
            EXPECT_EQ(sizeOfImage(e.out / "images" / name), (std::array<int, 2>{64, 48}));

        const auto cameras = readCameras(e.sparse("cameras.bin"));
        ASSERT_EQ(cameras.size(), 1u);
        EXPECT_EQ(cameras[0].width, 64u);
        EXPECT_EQ(cameras[0].height, 48u);

        // The same point lands on the same place of the smaller image: a tenth of the pixels
        const auto images = readImages(e.sparse("images.bin"));
        const Vec3 offset = {e.spec.offsetEasting, e.spec.offsetNorthing, 0.0};
        for (size_t s = 0; s < 2; ++s)
        {
            const Vec3 pc = {1.5, -0.7, 12};
            const Vec3 world = worldPoint(e.spec.shots[s], pc);
            const auto expected = odmPixel(e.spec.cameras[0].params, pc);
            const auto actual =
                colmapPixel(cameras[0], images[s], {world[0] - offset[0], world[1] - offset[1], world[2] - offset[2]});
            EXPECT_NEAR(actual[0], expected[0] / 10.0, 1e-6);
            EXPECT_NEAR(actual[1], expected[1] / 10.0, 1e-6);
        }
    }

    TEST(SplatTrainingExport, anImageNotLargerThanTheLimitKeepsItsSizeAndName)
    {
        Exported e(TEST_NAME);
        e.write();
        SplatTrainingExportOptions options;
        options.maxImageSize = 1000;
        e.run(options);
        EXPECT_EQ(sizeOfImage(e.out / "images" / "a.JPG"), (std::array<int, 2>{640, 480}));
        EXPECT_EQ(readCameras(e.sparse("cameras.bin"))[0].width, 640u);
    }

    // ---- The cameras ----

    TEST(SplatTrainingExport, foldsTheThirdRadialTermIntoTheOthersWithAWarning)
    {
        Exported e(TEST_NAME);
        e.spec.cameras = {brownCamera("test cam 640 480 brown 0.8", 0.02)};
        e.write();
        const auto result = e.run();
        EXPECT_TRUE(hasWarning(result, "k3")) << allWarnings(result);

        const auto cameras = readCameras(e.sparse("cameras.bin"));
        const auto images = readImages(e.sparse("images.bin"));
        const Vec3 offset = {e.spec.offsetEasting, e.spec.offsetNorthing, 0.0};
        // Without k3 the corners would be tens of pixels off; folded, they are within a pixel
        double worst = 0.0;
        for (const Vec3 &pc : std::vector<Vec3>{{0, 0, 10}, {5.0, 3.7, 10}, {-5.0, -3.7, 10}, {3, -2, 6}, {-6, 1, 12}})
        {
            const Vec3 world = worldPoint(e.spec.shots[0], pc);
            const auto expected = odmPixel(e.spec.cameras[0].params, pc);
            const auto actual =
                colmapPixel(cameras[0], images[0], {world[0] - offset[0], world[1] - offset[1], world[2] - offset[2]});
            worst = std::max({worst, std::abs(actual[0] - expected[0]), std::abs(actual[1] - expected[1])});
        }
        EXPECT_LT(worst, 1.0);
    }

    TEST(SplatTrainingExport, mapsPerspectiveAndFisheyeCameras)
    {
        Exported e(TEST_NAME);
        e.spec.cameras = {{"persp 640 480 perspective 0.8",
                           {{"projection_type", "perspective"}, {"width", 640}, {"height", 480}, {"focal", 0.9},
                            {"k1", -0.2}, {"k2", 0.04}}},
                          {"fish 640 480 fisheye_opencv 0.8",
                           {{"projection_type", "fisheye_opencv"}, {"width", 640}, {"height", 480}, {"focal_x", 0.8},
                            {"focal_y", 0.81}, {"c_x", 0.01}, {"c_y", -0.02}, {"k1", -0.05}, {"k2", -0.01},
                            {"k3", 0.001}, {"k4", 0.0002}}}};
        e.spec.shots[0].camera = "v2 persp 640 480 perspective 0.8";
        e.spec.shots[1].camera = "fish 640 480 fisheye_opencv 0.8"; // without the version prefix
        e.write();
        const auto result = e.run();
        EXPECT_EQ(result.images, 2u);

        const auto cameras = readCameras(e.sparse("cameras.bin"));
        ASSERT_EQ(cameras.size(), 2u);
        EXPECT_EQ(cameras[0].model, 3) << "RADIAL";
        EXPECT_EQ(cameras[0].params, (std::vector<double>{0.9 * 640, 320, 240, -0.2, 0.04}));
        EXPECT_EQ(cameras[1].model, 5) << "OPENCV_FISHEYE";
        EXPECT_EQ(cameras[1].params,
                  (std::vector<double>{0.8 * 640, 0.81 * 640, 320 + 0.01 * 640, 240 - 0.02 * 640, -0.05, -0.01, 0.001, 0.0002}));
        const auto images = readImages(e.sparse("images.bin"));
        EXPECT_EQ(images[0].camera, cameras[0].id);
        EXPECT_EQ(images[1].camera, cameras[1].id);
    }

    TEST(SplatTrainingExport, leavesOutTheImagesOfACameraCOLMAPHasNoModelFor)
    {
        Exported e(TEST_NAME);
        e.spec.cameras.push_back({"sphere 2000 1000 spherical 1",
                                  {{"projection_type", "spherical"}, {"width", 2000}, {"height", 1000}}});
        e.spec.shots[1].camera = "v2 sphere 2000 1000 spherical 1";
        e.spec.shots[1].width = 2000;
        e.spec.shots[1].height = 1000;
        e.write();
        const auto result = e.run();
        EXPECT_EQ(result.images, 1u);
        EXPECT_TRUE(hasWarning(result, "spherical")) << allWarnings(result);
        EXPECT_TRUE(hasWarning(result, "b.JPG")) << allWarnings(result);
        EXPECT_FALSE(fs::exists(e.out / "images" / "b.JPG"));
        EXPECT_EQ(readImages(e.sparse("images.bin")).size(), 1u);
    }

    TEST(SplatTrainingExport, matchesTheShotsOfSeveralCamerasByNameOrBySizeAndFocal)
    {
        Exported e(TEST_NAME);
        auto second = brownCamera("other cam 640 480 brown 0.85");
        second.params["focal_x"] = 0.85;
        second.params["focal_y"] = 0.85;
        auto third = brownCamera("small cam 320 240 brown 0.8");
        third.params["width"] = 320;
        third.params["height"] = 240;
        e.spec.cameras.push_back(second);
        e.spec.cameras.push_back(third);

        auto shot = [&](const std::string &name, const std::string &camera, int w, int h, double focal) {
            OdmShotSpec s = e.spec.shots[0];
            s.filename = name;
            s.camera = camera;
            s.width = w;
            s.height = h;
            s.focal = focal;
            return s;
        };
        e.spec.shots = {shot("named.JPG", "v2 other cam 640 480 brown 0.85", 640, 480, 0.85),
                        shot("by-focal.JPG", "", 640, 480, 0.85),
                        shot("by-size.JPG", "", 320, 240, 0.8),
                        shot("unknown-camera.JPG", "v2 not a camera", 640, 480, 0.8),
                        shot("undecidable.JPG", "", 640, 480, 0.7)};
        e.write();
        const auto result = e.run();

        // named and by-focal: the camera of focal 0.85; by-size: the small one; the camera that is
        // not in cameras.json falls back to the size and focal (the first camera); two cameras of the
        // size and none of the focal: left out
        const auto images = readImages(e.sparse("images.bin"));
        const auto cameras = readCameras(e.sparse("cameras.bin"));
        ASSERT_EQ(images.size(), 4u) << allWarnings(result);
        ASSERT_EQ(cameras.size(), 3u);
        std::map<std::string, Camera> cameraOf;
        for (const auto &i : images)
            for (const auto &c : cameras)
                if (c.id == i.camera)
                    cameraOf[i.name] = c;
        EXPECT_DOUBLE_EQ(cameraOf["named.JPG"].params[0], 0.85 * 640);
        EXPECT_DOUBLE_EQ(cameraOf["by-focal.JPG"].params[0], 0.85 * 640);
        EXPECT_EQ(cameraOf["by-size.JPG"].width, 320u);
        EXPECT_DOUBLE_EQ(cameraOf["unknown-camera.JPG"].params[0], 0.8 * 640);
        EXPECT_TRUE(hasWarning(result, "undecidable.JPG")) << allWarnings(result);
    }

    TEST(SplatTrainingExport, leavesOutAnImageThatIsNotTheSizeOfItsCameraOrIsMissing)
    {
        Exported e(TEST_NAME);
        e.spec.shots.push_back(e.spec.shots[0]);
        e.spec.shots.back().filename = "c.JPG";
        e.write();
        writeJpeg(e.project / "images" / "a.JPG", 480, 640); // turned, as a phone writes it
        fs::remove(e.project / "images" / "b.JPG");

        const auto result = e.run();
        EXPECT_EQ(result.images, 1u) << allWarnings(result);
        EXPECT_TRUE(hasWarning(result, "480x640")) << allWarnings(result);
        EXPECT_TRUE(hasWarning(result, "not in images/")) << allWarnings(result);
        EXPECT_EQ(readImages(e.sparse("images.bin"))[0].name, "c.JPG");
    }

    // ---- The nadir warning ----

    TEST(SplatTrainingExport, warnsWhenMostImagesLookStraightDown)
    {
        Exported e(TEST_NAME);
        // A rotation of pi about X turns the optical axis to nadir; a few degrees off still count
        e.spec.shots[0].rotation = {3.14159265358979, 0.0, 0.0};
        e.spec.shots[1].rotation = {3.14159265358979 - 0.1, 0.05, 0.0};
        e.write();
        const auto result = e.run();
        EXPECT_TRUE(hasWarning(result, "100% of the images look straight down")) << allWarnings(result);
    }

    TEST(SplatTrainingExport, doesNotWarnOfNadirWhenSomeImagesLookAside)
    {
        Exported e(TEST_NAME);
        e.spec.shots[0].rotation = {3.14159265358979, 0.0, 0.0};
        // Down at 30 degrees from nadir
        e.spec.shots[1].rotation = {3.14159265358979 - 0.5236, 0.0, 0.0};
        e.write();
        const auto result = e.run();
        EXPECT_FALSE(hasWarning(result, "straight down")) << allWarnings(result);
    }

    // Most is not enough: more than 98% of 100 images is 99
    TEST(SplatTrainingExport, warnsAboveNinetyEightPercentOnly)
    {
        Exported e(TEST_NAME);
        const OdmShotSpec down = [&] {
            OdmShotSpec s = e.spec.shots[0];
            s.rotation = {3.14159265358979, 0.0, 0.0};
            return s;
        }();
        e.spec.shots.clear();
        for (int i = 0; i < 100; ++i)
        {
            OdmShotSpec s = down;
            s.filename = "i" + std::to_string(i) + ".JPG";
            if (i >= 98)
                s.rotation = {1.6, 0.0, 0.0};
            e.spec.shots.push_back(s);
        }
        e.write();
        EXPECT_FALSE(hasWarning(e.run(), "straight down")) << "98 of 100 is not more than 98%";

        for (int i = 98; i < 100; ++i)
            e.spec.shots[i].rotation = i == 98 ? Vec3{3.14159265358979, 0, 0} : Vec3{1.6, 0, 0};
        e.write();
        EXPECT_TRUE(hasWarning(e.run(), "99% of the images look straight down"));
    }

    // ---- Errors, progress and cancellation ----

    TEST(SplatTrainingExport, nameTheInputThatIsMissing)
    {
        for (const auto &missing : std::vector<std::pair<fs::path, std::string>>{
                 {fs::path("odm_georeferencing") / "coords.txt", "coords.txt"},
                 {"cameras.json", "cameras.json"},
                 {fs::path("odm_report") / "shots.geojson", "shots.geojson"},
                 {"images", "images"}})
        {
            SCOPED_TRACE(missing.second);
            Exported e(TEST_NAME + std::string("-") + missing.second);
            e.write();
            fs::remove_all(e.project / missing.first);
            try
            {
                e.run();
                FAIL() << "no error";
            }
            catch (const AppException &ex)
            {
                EXPECT_NE(std::string(ex.what()).find(missing.second), std::string::npos) << ex.what();
            }
            EXPECT_FALSE(fs::exists(e.out / "sparse")) << "nothing is written for an input that is missing";
        }
    }

    TEST(SplatTrainingExport, refusesACoordsFileItCannotRead)
    {
        Exported e(TEST_NAME);
        e.write();
        writeTextFile(e.project / "odm_georeferencing" / "coords.txt", "WGS84 UTM 15N\nnot numbers\n");
        EXPECT_THROW(e.run(), AppException);
    }

    TEST(SplatTrainingExport, refusesAFolderThatIsNotAProjectOrIsTheOutput)
    {
        Exported e(TEST_NAME);
        e.write();
        EXPECT_THROW(exportSplatTrainingData(e.area.getPath("no-such-folder"), e.out), InvalidArgsException);
        EXPECT_THROW(exportSplatTrainingData(e.project, e.project), InvalidArgsException);
        SplatTrainingExportOptions options;
        options.maxImageSize = -1;
        EXPECT_THROW(exportSplatTrainingData(e.project, e.out, options), InvalidArgsException);
    }

    TEST(SplatTrainingExport, failsWhenNoImageCanBeExported)
    {
        Exported e(TEST_NAME);
        e.spec.cameras = {{"sphere 640 480 spherical 1", {{"projection_type", "spherical"}, {"width", 640}, {"height", 480}}}};
        e.spec.shots[0].camera = "sphere 640 480 spherical 1";
        e.spec.shots[1].camera = "sphere 640 480 spherical 1";
        e.write();
        try
        {
            e.run();
            FAIL() << "no error";
        }
        catch (const AppException &ex)
        {
            EXPECT_NE(std::string(ex.what()).find("spherical"), std::string::npos) << ex.what();
        }
    }

    TEST(SplatTrainingExport, reportsProgressFromZeroToOne)
    {
        Exported e(TEST_NAME);
        e.write();
        std::vector<std::pair<double, std::string>> seen;
        SplatTrainingExportOptions options;
        options.progress = [&](double fraction, const char *phase) {
            seen.emplace_back(fraction, phase);
            return false;
        };
        e.run(options);
        ASSERT_FALSE(seen.empty());
        EXPECT_EQ(seen.front().first, 0.0);
        EXPECT_EQ(seen.back().first, 1.0);
        std::set<std::string> phases;
        double last = 0.0;
        for (const auto &[fraction, phase] : seen)
        {
            EXPECT_GE(fraction, last) << phase;
            last = fraction;
            phases.insert(phase);
        }
        EXPECT_TRUE(phases.count("cameras") && phases.count("points") && phases.count("images") && phases.count("model"));
    }

    TEST(SplatTrainingExport, stopsWhenTheProgressCallbackAsksTo)
    {
        for (const char *stopAt : {"cameras", "points", "images"})
        {
            SCOPED_TRACE(stopAt);
            Exported e(TEST_NAME + std::string("-") + stopAt);
            e.write();
            SplatTrainingExportOptions options;
            options.progress = [&](double, const char *phase) { return std::string(phase) == stopAt; };
            EXPECT_THROW(e.run(options), CanceledException);
            EXPECT_FALSE(fs::exists(e.sparse("cameras.bin"))) << "a canceled export does not write the model";
        }
    }
} // namespace
