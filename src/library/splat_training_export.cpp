/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "splat_training_export.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <regex>
#include <set>
#include <unordered_map>

#include "pdal_inc.h"
#include <pdal/Options.hpp>
#include <pdal/Stage.hpp>
#include <pdal/StageFactory.hpp>

#include "cancellation.h"
#include "copc_utils.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "json.h"
#include "local_frame_georef.h"
#include "mio.h"
#include "parallel.h"
#include "ply.h"
#include "stringutil.h"

// The binary files of a COLMAP model are little-endian and are written from memory as they are
static_assert(std::endian::native == std::endian::little, "the COLMAP writer assumes a little-endian host");

namespace ddb
{
    namespace
    {
        using Vec3 = std::array<double, 3>;

        // An image looks straight down when its optical axis is within this angle of nadir
        constexpr double NadirAngleDegrees = 12.0;
        // Most of the images (more than this share) looking down is worth a warning
        constexpr double NadirShareToWarn = 0.98;
        // The cell of the thinning grid starts at this size (meters) and grows by this factor while there
        // are too many points: the cube root of 2, so that the points of a surface (which fall as the
        // square of the cell) fall by about a third, not by 3/4, and the grid ends close to the limit
        constexpr double InitialCellSize = 0.001;
        constexpr double CellGrowth = 1.25;
        // How many images are exported between two checks of the progress
        constexpr size_t ImageBatch = 16;
        constexpr size_t ImageThreads = 8;
        constexpr int JpegQuality = 95;
        // Files named in a warning
        constexpr size_t NamesInWarning = 3;

        // COLMAP camera models (colmap/src/colmap/sensor/models.h)
        enum ColmapModel : int32_t
        {
            ModelSimpleRadial = 2,
            ModelRadial = 3,
            ModelOpenCv = 4,
            ModelOpenCvFisheye = 5
        };

        size_t paramCount(int32_t model)
        {
            switch (model)
            {
            case ModelRadial:
                return 5;
            case ModelOpenCv:
            case ModelOpenCvFisheye:
                return 8;
            default:
                return 4;
            }
        }

        // ---- Progress and cancellation ----

        // One cancellation for the whole export: the callback may ask to stop, and then every
        // checkpoint of the call (the workers, GDAL) stops, as it does for a canceled token
        class Progress
        {
        public:
            explicit Progress(const std::function<bool(double, const char *)> &callback) : callback_(callback) {}

            // Reports progress, then stops the export when it was canceled. Safe from the workers.
            void report(double fraction, const char *phase)
            {
                {
                    const std::lock_guard lock(mutex_);
                    if (callback_ && callback_(std::clamp(fraction, 0.0, 1.0), phase))
                        callToken_.cancel("Export canceled by the progress callback");
                }
                cancel::throwIfCanceled();
            }

        private:
            const std::function<bool(double, const char *)> &callback_;
            std::mutex mutex_;
            cancel::CallToken callToken_;
        };

        // The share of the work each phase takes
        constexpr double CamerasEnd = 0.05;
        constexpr double PointsEnd = 0.40;
        constexpr double ImagesEnd = 0.95;

        double within(double begin, double end, double done)
        {
            return begin + (end - begin) * std::clamp(done, 0.0, 1.0);
        }

        // done of total, 0 when there is no total
        double shareOf(uint64_t done, uint64_t total)
        {
            return total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
        }

        // ---- Warnings ----

        // Why files were left out, one warning per reason with the first names
        class Skipped
        {
        public:
            void add(const std::string &reason, const std::string &name) { byReason_[reason].push_back(name); }

            void appendTo(std::vector<std::string> &warnings) const
            {
                for (const auto &[reason, names] : byReason_)
                {
                    std::string text = std::to_string(names.size()) + (names.size() == 1 ? " image" : " images") +
                                       " left out, " + reason + " (";
                    for (size_t i = 0; i < names.size() && i < NamesInWarning; ++i)
                        text += (i > 0 ? ", " : "") + names[i];
                    if (names.size() > NamesInWarning)
                        text += ", ...";
                    warnings.push_back(text + ")");
                }
            }

        private:
            std::map<std::string, std::vector<std::string>> byReason_;
        };

        // ---- The ODM project ----

        json readJsonFile(const fs::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in.good())
                throw AppException("Cannot read " + io::toUtf8(file));
            try
            {
                return json::parse(in);
            }
            catch (const json::exception &e)
            {
                throw AppException(io::toUtf8(file) + " is not valid JSON: " + e.what());
            }
        }

        fs::path requireFile(const fs::path &file, const std::string &what)
        {
            std::error_code ec;
            if (!fs::is_regular_file(file, ec))
                throw AppException("The ODM project has no " + what + " (" + io::toUtf8(file) +
                                           " is missing): the training data comes from a georeferenced ODM project");
            return file;
        }

        std::optional<double> numberOf(const json &j, const char *key)
        {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number())
                return std::nullopt;
            return it->get<double>();
        }

        std::optional<Vec3> vec3Of(const json &j, const char *key)
        {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_array() || it->size() != 3)
                return std::nullopt;
            Vec3 v{};
            for (size_t i = 0; i < 3; ++i)
            {
                if (!(*it)[i].is_number())
                    return std::nullopt;
                v[i] = (*it)[i].get<double>();
            }
            if (!std::all_of(v.begin(), v.end(), [](double d) { return std::isfinite(d); }))
                return std::nullopt;
            return v;
        }

        // A camera of cameras.json: its parameters are normalized by the longest side of the image
        // (OpenSfM), so they hold at any resolution
        struct OdmCamera
        {
            std::string id;
            std::string projection;
            int width = 0;
            int height = 0;
            json params;

            double focal() const { return numberOf(params, "focal_x").value_or(numberOf(params, "focal").value_or(0.0)); }
            double param(const char *key) const { return numberOf(params, key).value_or(0.0); }
        };

        std::vector<OdmCamera> readCameras(const fs::path &file)
        {
            const json cameras = readJsonFile(file);
            if (!cameras.is_object())
                throw AppException(io::toUtf8(file) + " is not an object of cameras");

            std::vector<OdmCamera> result;
            for (const auto &[id, camera] : cameras.items())
            {
                if (!camera.is_object())
                    continue;
                OdmCamera c;
                c.id = id;
                c.projection = camera.value("projection_type", std::string());
                c.width = static_cast<int>(numberOf(camera, "width").value_or(0));
                c.height = static_cast<int>(numberOf(camera, "height").value_or(0));
                c.params = camera;
                result.push_back(std::move(c));
            }
            if (result.empty())
                throw AppException(io::toUtf8(file) + " has no camera");
            return result;
        }

        // A shot of shots.geojson: the camera centre (E, N, Z in the CRS of the project, absolute)
        // and the rotation from the world to the camera as an axis-angle vector (OpenSfM)
        struct OdmShot
        {
            std::string filename;
            std::string camera;
            int width = 0;
            int height = 0;
            double focal = 0.0;
            Vec3 centre{};
            Vec3 rotation{};
        };

        std::vector<OdmShot> readShots(const fs::path &file, Skipped &skipped)
        {
            const json shots = readJsonFile(file);
            if (!shots.is_object() || !shots.contains("features") || !shots["features"].is_array())
                throw AppException(io::toUtf8(file) + " is not a GeoJSON FeatureCollection");

            std::vector<OdmShot> result;
            for (const auto &feature : shots["features"])
            {
                const auto props = feature.find("properties");
                if (props == feature.end() || !props->is_object())
                    continue;
                OdmShot s;
                s.filename = props->value("filename", std::string());
                const auto centre = vec3Of(*props, "translation");
                const auto rotation = vec3Of(*props, "rotation");
                if (s.filename.empty() || !centre || !rotation)
                {
                    skipped.add("its shot has no file name, translation or rotation", s.filename.empty() ? "?" : s.filename);
                    continue;
                }
                s.centre = *centre;
                s.rotation = *rotation;
                s.camera = props->value("camera", std::string());
                s.width = static_cast<int>(numberOf(*props, "width").value_or(0));
                s.height = static_cast<int>(numberOf(*props, "height").value_or(0));
                s.focal = numberOf(*props, "focal").value_or(0.0);
                result.push_back(std::move(s));
            }
            if (result.empty())
                throw AppException(io::toUtf8(file) + " has no shot");
            return result;
        }

        // The camera of a shot: the one it names (shots.geojson prefixes the id of cameras.json with
        // the version of the reconstruction, "v2 "), else the one of its size whose focal is its own
        const OdmCamera *cameraOfShot(const std::vector<OdmCamera> &cameras, const OdmShot &shot)
        {
            static const std::regex versionPrefix(R"(^v\d+\s+)");
            if (!shot.camera.empty())
            {
                const std::string bare = std::regex_replace(shot.camera, versionPrefix, "");
                for (const auto &c : cameras)
                    if (c.id == shot.camera || c.id == bare)
                        return &c;
            }

            std::vector<const OdmCamera *> sameSize;
            for (const auto &c : cameras)
                if (c.width == shot.width && c.height == shot.height)
                    sameSize.push_back(&c);
            if (sameSize.size() == 1)
                return sameSize.front();
            if (sameSize.empty() && cameras.size() == 1 && shot.width == 0 && shot.height == 0)
                return &cameras.front();

            // Several cameras of this size: the one whose focal is the shot's, if only one is
            const OdmCamera *best = nullptr;
            for (const auto *c : sameSize)
            {
                if (shot.focal > 0.0 && std::abs(c->focal() - shot.focal) <= 1e-6 * shot.focal)
                {
                    if (best != nullptr)
                        return nullptr; // two cameras alike: it is not known which one
                    best = c;
                }
            }
            return best;
        }

        // ---- Lens models ----

        struct ColmapCamera
        {
            int32_t model = ModelOpenCv;
            uint64_t width = 0;
            uint64_t height = 0;
            std::vector<double> params;
        };

        // OPENCV has radial terms k1 and k2 where the Brown model of ODM has k3 too. The radial
        // displacement of a point at radius r (in focal lengths) is k1 r^3 + k2 r^5 + k3 r^7; k1 and
        // k2 are refitted so that the displacement without k3 is the least-squares fit of the one
        // with it over the radius of the image (a plain drop of k3 moves the corners by tens of
        // pixels). Returns the largest remaining gap in pixels.
        double foldBrownK3(double focalPx, double maxRadius, double &k1, double &k2, double k3)
        {
            const double R = maxRadius;
            const auto pw = [&](int n) { return std::pow(R, n); };
            // Normal equations of the fit of a r^3 + b r^5 to k1 r^3 + k2 r^5 + k3 r^7 over [0, R]
            const double a11 = pw(7) / 7.0, a12 = pw(9) / 9.0, a22 = pw(11) / 11.0;
            const double b1 = k1 * pw(7) / 7.0 + k2 * pw(9) / 9.0 + k3 * pw(11) / 11.0;
            const double b2 = k1 * pw(9) / 9.0 + k2 * pw(11) / 11.0 + k3 * pw(13) / 13.0;
            const double det = a11 * a22 - a12 * a12;
            const double fitted1 = (b1 * a22 - b2 * a12) / det;
            const double fitted2 = (a11 * b2 - a12 * b1) / det;

            double gap = 0.0;
            for (int i = 1; i <= 64; ++i)
            {
                const double r = R * i / 64.0;
                const double r3 = r * r * r;
                const double was = k1 * r3 + k2 * r3 * r * r + k3 * r3 * r3 * r;
                const double now = fitted1 * r3 + fitted2 * r3 * r * r;
                gap = std::max(gap, std::abs(was - now));
            }
            k1 = fitted1;
            k2 = fitted2;
            return gap * focalPx;
        }

        // The COLMAP camera of an ODM camera for an image of width x height pixels (the ODM
        // parameters are normalized, so the camera of a resized image is the same camera). Nothing
        // when the projection has no COLMAP model; a note when the model approximates the lens.
        std::optional<ColmapCamera> colmapCameraOf(const OdmCamera &odm, int width, int height, std::string &note)
        {
            const double side = std::max(width, height);
            ColmapCamera c;
            c.width = static_cast<uint64_t>(width);
            c.height = static_cast<uint64_t>(height);

            if (odm.projection == "brown")
            {
                const double fx = odm.param("focal_x") * side;
                const double fy = odm.param("focal_y") * side;
                double k1 = odm.param("k1"), k2 = odm.param("k2");
                const double k3 = odm.param("k3");
                if (!(fx > 0.0) || !(fy > 0.0))
                    return std::nullopt;
                if (k3 != 0.0)
                {
                    const double radius = 0.5 * std::hypot(width, height) / fx;
                    const double gap = foldBrownK3(fx, radius, k1, k2, k3);
                    if (gap > 0.05)
                        note = "camera '" + odm.id + "': the Brown k3 (" + std::to_string(k3) +
                               ") has no place in COLMAP's OPENCV model, so it is folded into k1 and k2, which "
                               "keeps the lens within " +
                               std::to_string(gap).substr(0, 4) + " px of ODM's";
                }
                c.model = ModelOpenCv;
                c.params = {fx,
                            fy,
                            width / 2.0 + odm.param("c_x") * side,
                            height / 2.0 + odm.param("c_y") * side,
                            k1,
                            k2,
                            odm.param("p1"),
                            odm.param("p2")};
                return c;
            }
            if (odm.projection == "perspective")
            {
                const double f = odm.param("focal") * side;
                if (!(f > 0.0))
                    return std::nullopt;
                c.model = ModelRadial;
                c.params = {f, width / 2.0, height / 2.0, odm.param("k1"), odm.param("k2")};
                return c;
            }
            if (odm.projection == "fisheye_opencv")
            {
                const double fx = odm.param("focal_x") * side;
                const double fy = odm.param("focal_y") * side;
                if (!(fx > 0.0) || !(fy > 0.0))
                    return std::nullopt;
                c.model = ModelOpenCvFisheye;
                c.params = {fx,
                            fy,
                            width / 2.0 + odm.param("c_x") * side,
                            height / 2.0 + odm.param("c_y") * side,
                            odm.param("k1"),
                            odm.param("k2"),
                            odm.param("k3"),
                            odm.param("k4")};
                return c;
            }
            return std::nullopt;
        }

        // ---- Poses ----

        // The pose of a COLMAP image: the rotation from the world to the camera as a unit
        // quaternion (w, x, y, z) and the translation, so that a point P of the world is
        // R * P + t in the camera, whose axes are X right, Y down and Z forward (those of OpenSfM)
        struct Pose
        {
            std::array<double, 4> q{1, 0, 0, 0};
            Vec3 t{};
            Vec3 viewDirection{}; // the optical axis in the world
            Vec3 centre{};
        };

        // centre is the camera centre in the local frame; rotation the axis-angle of the rotation
        // from the world to the camera
        Pose poseOf(const Vec3 &centre, const Vec3 &rotation)
        {
            const double angle = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2]);
            Vec3 k{0, 0, 1};
            if (angle > 1e-12)
                k = {rotation[0] / angle, rotation[1] / angle, rotation[2] / angle};

            // Rodrigues: R = cos I + (1 - cos) k k^T + sin [k]x
            const double c = std::cos(angle), s = std::sin(angle), v = 1.0 - c;
            const std::array<std::array<double, 3>, 3> R = {{
                {c + k[0] * k[0] * v, k[0] * k[1] * v - k[2] * s, k[0] * k[2] * v + k[1] * s},
                {k[1] * k[0] * v + k[2] * s, c + k[1] * k[1] * v, k[1] * k[2] * v - k[0] * s},
                {k[2] * k[0] * v - k[1] * s, k[2] * k[1] * v + k[0] * s, c + k[2] * k[2] * v},
            }};

            Pose p;
            const double half = angle / 2.0;
            const double sign = std::cos(half) < 0.0 ? -1.0 : 1.0; // w >= 0
            p.q = {sign * std::cos(half), sign * k[0] * std::sin(half), sign * k[1] * std::sin(half),
                   sign * k[2] * std::sin(half)};
            for (size_t i = 0; i < 3; ++i)
                p.t[i] = -(R[i][0] * centre[0] + R[i][1] * centre[1] + R[i][2] * centre[2]);
            p.viewDirection = {R[2][0], R[2][1], R[2][2]};
            p.centre = centre;
            return p;
        }

        // ---- The point cloud ----

        struct SampledPoint
        {
            double x, y, z;
            uint16_t r, g, b;
        };

        // Thins a stream of points on a 3D grid down to at most `limit`: the first point of each
        // cell stays, and the cell grows while there are more cells than the limit. One
        // pass, no knowledge of the cloud (its size, its extent) and memory of the limit.
        class GridSampler
        {
        public:
            explicit GridSampler(size_t limit) : limit_(limit) { points_.reserve(std::min<size_t>(limit, 1u << 20)); }

            void add(double x, double y, double z, uint16_t r, uint16_t g, uint16_t b)
            {
                ++seen_;
                const auto key = keyOf(x, y, z);
                if (!key || limit_ == 0)
                    return;
                if (cells_.try_emplace(*key, static_cast<uint32_t>(points_.size())).second)
                {
                    points_.push_back({x, y, z, r, g, b});
                    if (points_.size() > limit_)
                        coarsen();
                }
            }

            size_t seen() const { return seen_; }
            const std::vector<SampledPoint> &points() const { return points_; }

        private:
            struct Key
            {
                int64_t x, y, z;
                bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
            };
            struct KeyHash
            {
                size_t operator()(const Key &k) const
                {
                    size_t h = std::hash<int64_t>()(k.x);
                    h = h * 0x9E3779B97F4A7C15ull + std::hash<int64_t>()(k.y);
                    h = h * 0x9E3779B97F4A7C15ull + std::hash<int64_t>()(k.z);
                    return h;
                }
            };

            // Nothing for a coordinate that is not finite or too far for the grid
            std::optional<Key> keyOf(double x, double y, double z) const
            {
                const double gx = std::floor(x / cell_), gy = std::floor(y / cell_), gz = std::floor(z / cell_);
                constexpr double Limit = 4e18;
                if (!(std::abs(gx) < Limit) || !(std::abs(gy) < Limit) || !(std::abs(gz) < Limit))
                    return std::nullopt;
                return Key{static_cast<int64_t>(gx), static_cast<int64_t>(gy), static_cast<int64_t>(gz)};
            }

            // Grows the cell until the points that stay fit; the first of the points that share a
            // cell stays
            void coarsen()
            {
                while (points_.size() > limit_)
                {
                    cell_ *= CellGrowth;
                    cells_.clear();
                    std::vector<SampledPoint> kept;
                    kept.reserve(points_.size());
                    for (const auto &p : points_)
                        if (cells_.try_emplace(*keyOf(p.x, p.y, p.z), static_cast<uint32_t>(kept.size())).second)
                            kept.push_back(p);
                    points_ = std::move(kept);
                }
            }

            size_t limit_;
            double cell_ = InitialCellSize;
            size_t seen_ = 0;
            std::unordered_map<Key, uint32_t, KeyHash> cells_;
            std::vector<SampledPoint> points_;
        };

        // The scalar at p as a double
        double plyScalar(const char *p, PlyScalarType type)
        {
            switch (type)
            {
            case PlyScalarType::Int8:
                return static_cast<double>(*reinterpret_cast<const int8_t *>(p));
            case PlyScalarType::UInt8:
                return static_cast<double>(*reinterpret_cast<const uint8_t *>(p));
            case PlyScalarType::Int16: {
                int16_t v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            case PlyScalarType::UInt16: {
                uint16_t v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            case PlyScalarType::Int32: {
                int32_t v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            case PlyScalarType::UInt32: {
                uint32_t v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            case PlyScalarType::Float32: {
                float v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            case PlyScalarType::Float64: {
                double v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            default:
                return 0.0;
            }
        }

        // The points of a binary little-endian PLY (ODM's filtered cloud), already in the local
        // frame. Colours are read by property name: the order of red, green and blue varies.
        void forEachPlyPoint(const fs::path &file, const std::function<void(PointBatch &)> &sink)
        {
            const PlyHeader header = readPlyHeader(file);
            if (header.format != PlyFormat::BinaryLittleEndian)
                throw AppException(io::toUtf8(file) + " is not a binary little-endian PLY");
            const int vertexIndex = header.findElement("vertex");
            if (vertexIndex < 0)
                throw AppException(io::toUtf8(file) + " has no vertex element");
            const PlyElement &vertex = header.elements[vertexIndex];
            if (vertexIndex != 0)
                throw AppException(io::toUtf8(file) + " has an element before its vertices");

            struct Column
            {
                size_t offset = 0;
                PlyScalarType type = PlyScalarType::Unknown;
                bool present = false;
            };
            Column x, y, z, red, green, blue;
            size_t stride = 0;
            for (const auto &p : vertex.properties)
            {
                if (p.isList || plyScalarSize(p.type) == 0)
                    throw AppException(io::toUtf8(file) + " has a vertex property '" + p.name +
                                               "' that is a list or of an unknown type");
                const Column column{stride, p.type, true};
                if (p.name == "x") x = column;
                else if (p.name == "y") y = column;
                else if (p.name == "z") z = column;
                else if (p.name == "red") red = column;
                else if (p.name == "green") green = column;
                else if (p.name == "blue") blue = column;
                stride += plyScalarSize(p.type);
            }
            if (!x.present || !y.present || !z.present || stride == 0)
                throw AppException(io::toUtf8(file) + " has no x, y and z vertex properties");

            std::ifstream in(file, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(header.headerLength));
            constexpr size_t RowsPerBatch = 16384;
            std::vector<char> rows(RowsPerBatch * stride);
            PointBatch batch;
            batch.reserve(RowsPerBatch);
            const bool hasColor = red.present && green.present && blue.present;
            for (uint64_t done = 0; done < vertex.count;)
            {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(RowsPerBatch, vertex.count - done));
                in.read(rows.data(), static_cast<std::streamsize>(n * stride));
                if (static_cast<size_t>(in.gcount()) != n * stride)
                    throw AppException(io::toUtf8(file) + " ends before its " + std::to_string(vertex.count) +
                                               " vertices");
                batch.clear();
                for (size_t i = 0; i < n; ++i)
                {
                    const char *row = rows.data() + i * stride;
                    batch.x.push_back(plyScalar(row + x.offset, x.type));
                    batch.y.push_back(plyScalar(row + y.offset, y.type));
                    batch.z.push_back(plyScalar(row + z.offset, z.type));
                    batch.red.push_back(hasColor ? static_cast<uint16_t>(plyScalar(row + red.offset, red.type)) : 0);
                    batch.green.push_back(hasColor ? static_cast<uint16_t>(plyScalar(row + green.offset, green.type)) : 0);
                    batch.blue.push_back(hasColor ? static_cast<uint16_t>(plyScalar(row + blue.offset, blue.type)) : 0);
                }
                sink(batch);
                done += n;
            }
        }

        // The points of a LAS or LAZ file, one batch at a time
        void forEachLasPoint(const fs::path &file, const std::function<void(PointBatch &)> &sink, uint64_t &total)
        {
            pdal::StageFactory factory;
            pdal::Stage *reader = factory.createStage("readers.las");
            if (reader == nullptr)
                throw PDALException("Cannot create the LAS reader");
            pdal::Options options;
            options.add("filename", io::toUtf8(file));
            options.add("nosrs", true);
            reader->setOptions(options);
            try
            {
                total = reader->preview().m_pointCount;
                forEachPointBatch(*reader, sink);
            }
            catch (const pdal::pdal_error &e)
            {
                throw PDALException("Cannot read " + io::toUtf8(file) + ": " + e.what());
            }
        }

        // The cloud to take the points of and whether its coordinates are those of the CRS (a LAS
        // file) or already shifted by the offset (ODM's filtered PLY)
        struct CloudFile
        {
            fs::path path;
            bool absolute = false;
            bool las = false;
        };

        std::optional<CloudFile> findCloud(const fs::path &project)
        {
            const std::array<CloudFile, 3> candidates = {{
                {project / "odm_georeferencing" / "odm_georeferenced_model.laz", true, true},
                {project / "odm_georeferencing" / "odm_georeferenced_model.las", true, true},
                {project / "odm_filterpoints" / "point_cloud.ply", false, false},
            }};
            for (const auto &c : candidates)
            {
                std::error_code ec;
                if (fs::is_regular_file(c.path, ec))
                    return c;
            }
            return std::nullopt;
        }

        struct Cloud
        {
            std::vector<SampledPoint> points;
            bool hasColor = false;
            bool wide = false; // colours are 16-bit
        };

        // The points of the cloud thinned to at most `limit`, in the local frame
        Cloud sampleCloud(const CloudFile &cloud, const Vec3 &offset, size_t limit, Progress &progress)
        {
            GridSampler sampler(limit);
            uint64_t total = 0;
            size_t read = 0;
            const Vec3 shift = cloud.absolute ? offset : Vec3{0, 0, 0};
            const auto sink = [&](PointBatch &batch) {
                for (size_t i = 0; i < batch.size(); ++i)
                    sampler.add(batch.x[i] - shift[0], batch.y[i] - shift[1], batch.z[i] - shift[2], batch.red[i],
                                batch.green[i], batch.blue[i]);
                read += batch.size();
                progress.report(within(CamerasEnd, PointsEnd, shareOf(read, total)),
                                "points");
            };

            if (cloud.las)
                forEachLasPoint(cloud.path, sink, total);
            else
                forEachPlyPoint(cloud.path, sink);

            Cloud result;
            result.points = sampler.points();
            for (const auto &p : result.points)
            {
                result.hasColor = result.hasColor || p.r != 0 || p.g != 0 || p.b != 0;
                result.wide = result.wide || p.r > 255 || p.g > 255 || p.b > 255;
            }
            return result;
        }

        // ---- Images ----

        struct ImageSize
        {
            int width = 0;
            int height = 0;
        };

        ImageSize imageSizeOf(const fs::path &file)
        {
            const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
            if (!ds)
                throw GDALException("Cannot open " + io::toUtf8(file));
            return {GDALGetRasterXSize(datasetHandle(ds)), GDALGetRasterYSize(datasetHandle(ds))};
        }

        // The size of an image once its longest side is at most maxSize; 0 keeps it
        ImageSize resizedTo(const ImageSize &size, int maxSize)
        {
            const int side = std::max(size.width, size.height);
            if (maxSize <= 0 || side <= maxSize)
                return size;
            const double scale = static_cast<double>(maxSize) / side;
            return {std::max(1, static_cast<int>(std::lround(size.width * scale))),
                    std::max(1, static_cast<int>(std::lround(size.height * scale)))};
        }

        // Writes the image resampled to a JPEG of the given size
        void resizeToJpeg(const fs::path &source, const fs::path &target, const ImageSize &size)
        {
            const auto src = adoptDataset(GDALOpen(io::toUtf8(source).c_str(), GA_ReadOnly));
            if (!src)
                throw GDALException("Cannot open " + io::toUtf8(source));
            GDALDatasetH hSrc = datasetHandle(src);
            const int bands = GDALGetRasterCount(hSrc);

            // No EXIF: the camera's metadata (its orientation among it) does not describe the resampled pixels
            std::vector<std::string> args = {"-of", "JPEG", "-co", "QUALITY=" + std::to_string(JpegQuality),
                                             "-co", "WRITE_EXIF_METADATA=NO",
                                             "-outsize", std::to_string(size.width), std::to_string(size.height),
                                             "-r", "average"};
            // JPEG holds bytes: 16-bit pixels (the bands of a multispectral camera) are scaled from the
            // range of their type, the same for every image, so the images keep their relative exposure
            const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(hSrc, 1));
            if (type == GDT_UInt16)
                args.insert(args.end(), {"-ot", "Byte", "-scale", "0", "65535", "0", "255"});
            else if (type == GDT_Int16)
                args.insert(args.end(), {"-ot", "Byte", "-scale", "0", "32767", "0", "255"});
            else if (type != GDT_Byte)
                throw GDALException(io::toUtf8(source) + " has " + GDALGetDataTypeName(type) +
                                    " pixels, which cannot be written as a JPEG");
            // JPEG holds 1 or 3 bands: grey, or the colours of an image with an alpha or more bands
            const int keep = bands == 2 ? 1 : std::min(bands, 3);
            if (keep != bands)
                for (int b = 1; b <= keep; ++b)
                    args.insert(args.end(), {"-b", std::to_string(b)});

            CPLStringList argv = cslFromStrings(args);
            const GdalOptionsPtr<GDALTranslateOptions> options(GDALTranslateOptionsNew(argv.List(), nullptr));
            if (!options)
                throw GDALException("Cannot resize " + io::toUtf8(source));
            cancel::GdalProgress progress;
            progress.attach(options.get());

            auto out = adoptDataset(GDALTranslate(io::toUtf8(target).c_str(), hSrc, options.get(), nullptr));
            if (!out)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot resize " + io::toUtf8(source) + ": " + CPLGetLastErrorMsg());
            }
            if (out->Close() != CE_None)
                throw GDALException("Cannot write " + io::toUtf8(target) + ": " + CPLGetLastErrorMsg());
        }

        bool isJpegName(const std::string &name)
        {
            return str::endsWithCI(name, ".jpg") || str::endsWithCI(name, ".jpeg");
        }

        bool isPlainFileName(const std::string &name)
        {
            return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
                   name.find('\\') == std::string::npos;
        }

        // ---- The model ----

        // An image the model will name: where it comes from, what it is called and the camera it
        // was taken with
        struct PlannedImage
        {
            std::string name;
            fs::path source;
            const OdmCamera *camera = nullptr;
            ImageSize size{};    // of the original
            ImageSize target{};  // of the exported file
            Pose pose;
            bool exported = false;
        };

        // The writer of the binary files of a COLMAP model
        class BinaryWriter
        {
        public:
            template <typename T>
            void put(T value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                data_.append(reinterpret_cast<const char *>(&value), sizeof(T));
            }
            void putString(const std::string &text)
            {
                data_.append(text);
                data_.push_back('\0');
            }
            const std::string &data() const { return data_; }

        private:
            std::string data_;
        };

        std::string camerasBin(const std::vector<std::pair<uint32_t, ColmapCamera>> &cameras)
        {
            BinaryWriter w;
            w.put<uint64_t>(cameras.size());
            for (const auto &[id, c] : cameras)
            {
                w.put<uint32_t>(id);
                w.put<int32_t>(c.model);
                w.put<uint64_t>(c.width);
                w.put<uint64_t>(c.height);
                for (size_t i = 0; i < paramCount(c.model); ++i)
                    w.put<double>(c.params[i]);
            }
            return w.data();
        }

        std::string imagesBin(const std::vector<PlannedImage> &images, const std::map<const OdmCamera *, uint32_t> &cameraIds)
        {
            BinaryWriter w;
            w.put<uint64_t>(images.size());
            uint32_t id = 0;
            for (const auto &image : images)
            {
                w.put<uint32_t>(++id);
                for (double q : image.pose.q)
                    w.put<double>(q);
                for (double t : image.pose.t)
                    w.put<double>(t);
                w.put<uint32_t>(cameraIds.at(image.camera));
                w.putString(image.name);
                w.put<uint64_t>(0); // no 2D points: a trainer needs the poses and the 3D points only
            }
            return w.data();
        }

        std::string points3dBin(const Cloud &cloud)
        {
            BinaryWriter w;
            w.put<uint64_t>(cloud.points.size());
            uint64_t id = 0;
            for (const auto &p : cloud.points)
            {
                w.put<uint64_t>(++id);
                w.put<double>(p.x);
                w.put<double>(p.y);
                w.put<double>(p.z);
                const auto channel = [&](uint16_t v) -> uint8_t {
                    if (!cloud.hasColor)
                        return 128; // grey: the cloud has no colours
                    return static_cast<uint8_t>(cloud.wide ? v >> 8 : v);
                };
                w.put<uint8_t>(channel(p.r));
                w.put<uint8_t>(channel(p.g));
                w.put<uint8_t>(channel(p.b));
                w.put<double>(0.0); // reprojection error
                w.put<uint64_t>(0); // track length: no image observes it
            }
            return w.data();
        }

        // Whether the cloud and the cameras are in the same place: their extents (horizontal)
        // meet, give or take half the extent of the cameras
        bool cloudMeetsCameras(const Cloud &cloud, const std::vector<PlannedImage> &images)
        {
            if (cloud.points.empty() || images.empty())
                return true;
            double cMinX = INFINITY, cMaxX = -INFINITY, cMinY = INFINITY, cMaxY = -INFINITY;
            for (const auto &i : images)
            {
                cMinX = std::min(cMinX, i.pose.centre[0]);
                cMaxX = std::max(cMaxX, i.pose.centre[0]);
                cMinY = std::min(cMinY, i.pose.centre[1]);
                cMaxY = std::max(cMaxY, i.pose.centre[1]);
            }
            const double margin = std::max({50.0, 0.5 * (cMaxX - cMinX), 0.5 * (cMaxY - cMinY)});
            for (const auto &p : cloud.points)
                if (p.x >= cMinX - margin && p.x <= cMaxX + margin && p.y >= cMinY - margin && p.y <= cMaxY + margin)
                    return true;
            return false;
        }

        // The images of the project that can be part of the model, with their camera and pose.
        // Those that cannot are counted in `skipped`.
        std::vector<PlannedImage> planImages(const fs::path &project, const std::vector<OdmShot> &shots,
                                             const std::vector<OdmCamera> &cameras, const LocalFrameGeoref &georef,
                                             int maxImageSize, Skipped &skipped, std::vector<std::string> &warnings,
                                             Progress &progress)
        {
            std::vector<PlannedImage> planned;
            std::set<std::string> names;
            std::set<std::string> noted;
            for (size_t i = 0; i < shots.size(); ++i)
            {
                progress.report(within(0.0, CamerasEnd, shareOf(i, shots.size())), "cameras");
                const OdmShot &shot = shots[i];
                if (!isPlainFileName(shot.filename))
                {
                    skipped.add("its file name is not a plain file name", shot.filename);
                    continue;
                }
                const OdmCamera *camera = cameraOfShot(cameras, shot);
                if (camera == nullptr)
                {
                    skipped.add("its camera is not in cameras.json", shot.filename);
                    continue;
                }

                std::string note;
                if (!colmapCameraOf(*camera, std::max(camera->width, 1), std::max(camera->height, 1), note))
                {
                    skipped.add("COLMAP has no model for the '" + camera->projection + "' camera of ODM (brown, perspective and fisheye_opencv are supported)",
                                shot.filename);
                    continue;
                }

                PlannedImage image;
                image.camera = camera;
                image.source = project / "images" / io::fromUtf8(shot.filename);
                std::error_code ec;
                if (!fs::is_regular_file(image.source, ec))
                {
                    skipped.add("the file is not in images/", shot.filename);
                    continue;
                }
                try
                {
                    const ImageSize size = imageSizeOf(image.source);
                    if (size.width != camera->width || size.height != camera->height)
                    {
                        skipped.add("its size is " + std::to_string(size.width) + "x" + std::to_string(size.height) +
                                        ", not the " + std::to_string(camera->width) + "x" + std::to_string(camera->height) +
                                        " of its camera",
                                    shot.filename);
                        continue;
                    }
                    image.size = size;
                }
                catch (const GDALException &e)
                {
                    skipped.add(std::string("it cannot be read (") + e.what() + ")", shot.filename);
                    continue;
                }
                image.target = resizedTo(image.size, maxImageSize);
                const bool resized = image.target.width != image.size.width || image.target.height != image.size.height;
                image.name = resized && !isJpegName(shot.filename) ? shot.filename + ".jpg" : shot.filename;
                if (!names.insert(image.name).second)
                {
                    skipped.add("another shot has the same file name", shot.filename);
                    continue;
                }

                if (!note.empty() && noted.insert(camera->id).second)
                    warnings.push_back(note);

                const Vec3 centre = {shot.centre[0] - georef.offset[0], shot.centre[1] - georef.offset[1],
                                     shot.centre[2] - georef.offset[2]};
                image.pose = poseOf(centre, shot.rotation);
                planned.push_back(std::move(image));
            }
            return planned;
        }

        // Links or resizes the images into outFolder/images, a few at a time; an image that cannot
        // be written is left out (its `exported` stays false)
        void exportImages(std::vector<PlannedImage> &images, const fs::path &imagesFolder, Skipped &skipped,
                          Progress &progress)
        {
            for (size_t begin = 0; begin < images.size(); begin += ImageBatch)
            {
                progress.report(within(PointsEnd, ImagesEnd, shareOf(begin, images.size())), "images");
                const size_t end = std::min(images.size(), begin + ImageBatch);
                std::mutex failures;
                parallel::forEachIndex(end - begin, ImageThreads, [&](size_t k) {
                    PlannedImage &image = images[begin + k];
                    const fs::path target = imagesFolder / io::fromUtf8(image.name);
                    const bool resized =
                        image.target.width != image.size.width || image.target.height != image.size.height;
                    try
                    {
                        if (resized)
                            resizeToJpeg(image.source, target, image.target);
                        else
                            io::hardlinkSafe(image.source, target);
                        image.exported = true;
                    }
                    catch (const CanceledException &)
                    {
                        throw;
                    }
                    catch (const std::exception &e)
                    {
                        std::error_code ec;
                        fs::remove(target, ec);
                        const std::lock_guard lock(failures);
                        skipped.add(std::string("it cannot be written (") + e.what() + ")", image.name);
                    }
                });
            }
        }

        // The warning about a capture whose images mostly look down, or "" when it is not
        std::string nadirWarning(const std::vector<PlannedImage> &images)
        {
            if (images.empty())
                return "";
            const double limit = -std::cos(NadirAngleDegrees * std::numbers::pi / 180.0);
            const size_t down = static_cast<size_t>(std::count_if(
                images.begin(), images.end(), [&](const PlannedImage &i) { return i.pose.viewDirection[2] <= limit; }));
            const double share = shareOf(down, images.size());
            if (share <= NadirShareToWarn)
                return "";
            return std::to_string(static_cast<int>(std::lround(share * 100.0))) + "% of the images look straight down (within " +
                   std::to_string(static_cast<int>(NadirAngleDegrees)) +
                   " degrees of nadir): a splat trained from them is sharp from above and poor from any other angle; "
                   "images taken at an angle (oblique or orbiting the subject) are needed to see it from the side";
        }

        std::string georefJson(const LocalFrameGeoref &georef)
        {
            return json{{"crs", georef.crs}, {"offset", {georef.offset[0], georef.offset[1], georef.offset[2]}}}.dump(2) +
                   "\n";
        }

        // The ODM georeference of the project: its coords.txt, as detectLocalFrameGeoref reads it
        LocalFrameGeoref odmGeoref(const fs::path &coords)
        {
            const auto georef = detectLocalFrameGeoref(io::toUtf8(coords));
            if (!georef || georef->kind != LocalFrameGeoref::Kind::Projected)
                throw AppException("Cannot read the CRS and the offset of the project from " + io::toUtf8(coords) +
                                           ": its first line is the CRS (\"WGS84 UTM 15N\") and its second the "
                                           "easting and northing of the offset");
            return *georef;
        }
    } // namespace

    SplatTrainingExportResult exportSplatTrainingData(const fs::path &odmProjectFolder, const fs::path &outFolder,
                                                      const SplatTrainingExportOptions &options)
    {
        if (options.maxImageSize < 0)
            throw InvalidArgsException("The maximum image size cannot be negative");

        std::error_code ec;
        if (!fs::is_directory(odmProjectFolder, ec))
            throw InvalidArgsException(io::toUtf8(odmProjectFolder) + " is not a folder");
        if (io::isSamePath(odmProjectFolder, outFolder))
            throw InvalidArgsException("The training data cannot be written into the ODM project folder itself");

        // Everything it needs, before anything is written
        const fs::path coordsFile = requireFile(odmProjectFolder / "odm_georeferencing" / "coords.txt", "coords.txt");
        const fs::path camerasFile = requireFile(odmProjectFolder / "cameras.json", "cameras.json");
        const fs::path shotsFile = requireFile(odmProjectFolder / "odm_report" / "shots.geojson", "shots.geojson");
        if (!fs::is_directory(odmProjectFolder / "images", ec))
            throw AppException("The ODM project has no images folder (" +
                                       io::toUtf8(odmProjectFolder / "images") + " is missing)");

        SplatTrainingExportResult result;
        Progress progress(options.progress);
        progress.report(0.0, "cameras");

        const LocalFrameGeoref georef = odmGeoref(coordsFile);
        const std::vector<OdmCamera> cameras = readCameras(camerasFile);
        Skipped skipped;
        const std::vector<OdmShot> shots = readShots(shotsFile, skipped);

        std::vector<PlannedImage> images =
            planImages(odmProjectFolder, shots, cameras, georef, options.maxImageSize, skipped, result.warnings, progress);
        if (images.empty())
        {
            skipped.appendTo(result.warnings);
            std::string reasons;
            for (const auto &w : result.warnings)
                reasons += "\n  " + w;
            throw AppException("No image of the ODM project can be exported" + reasons);
        }

        // The points
        Cloud cloud;
        if (options.maxPoints == 0)
        {
            result.warnings.push_back("No points were asked for: points3D.bin is empty, so a trainer starts from random points");
        }
        else if (const auto file = findCloud(odmProjectFolder))
        {
            cloud = sampleCloud(*file, georef.offset, options.maxPoints, progress);
            if (cloud.points.empty())
            {
                result.warnings.push_back("The point cloud " + io::toUtf8(file->path) +
                                          " has no point: points3D.bin is empty, so a trainer starts from random points");
            }
            else if (!cloudMeetsCameras(cloud, images))
            {
                result.warnings.push_back("The point cloud " + io::toUtf8(file->path) +
                                          " lies away from the cameras, so it is probably not in the coordinates of "
                                          "the project: the points of the model will not match the images");
            }
        }
        else
        {
            result.warnings.push_back(
                "The ODM project has no point cloud (odm_georeferencing/odm_georeferenced_model.laz or "
                "odm_filterpoints/point_cloud.ply): points3D.bin is empty, so a trainer starts from random points");
        }

        // The images
        const fs::path imagesFolder = io::assureFolderExists(outFolder / "images");
        exportImages(images, imagesFolder, skipped, progress);
        images.erase(std::remove_if(images.begin(), images.end(), [](const PlannedImage &i) { return !i.exported; }),
                     images.end());
        skipped.appendTo(result.warnings);
        if (images.empty())
        {
            std::string reasons;
            for (const auto &w : result.warnings)
                reasons += "\n  " + w;
            throw AppException("No image of the ODM project can be exported" + reasons);
        }
        if (const auto warning = nadirWarning(images); !warning.empty())
            result.warnings.push_back(warning);

        // The model, and the georeference to put next to a splat trained from it
        progress.report(ImagesEnd, "model");
        std::vector<std::pair<uint32_t, ColmapCamera>> colmapCameras;
        std::map<const OdmCamera *, uint32_t> cameraIds;
        for (const auto &image : images)
        {
            if (cameraIds.count(image.camera) != 0)
                continue;
            std::string note;
            const auto camera = colmapCameraOf(*image.camera, image.target.width, image.target.height, note);
            const auto id = static_cast<uint32_t>(colmapCameras.size() + 1);
            colmapCameras.emplace_back(id, *camera);
            cameraIds[image.camera] = id;
        }

        const fs::path sparse = io::assureFolderExists(outFolder / "sparse" / "0");
        io::writeFileAtomic(sparse / "cameras.bin", camerasBin(colmapCameras), "cameras.bin");
        io::writeFileAtomic(sparse / "images.bin", imagesBin(images, cameraIds), "images.bin");
        io::writeFileAtomic(sparse / "points3D.bin", points3dBin(cloud), "points3D.bin");
        io::writeFileAtomic(outFolder / "georef.json", georefJson(georef), "georef.json");

        result.images = images.size();
        result.points = cloud.points.size();
        progress.report(1.0, "model");
        return result;
    }

} // namespace ddb
