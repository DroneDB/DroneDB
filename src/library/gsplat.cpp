/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gsplat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include "buildlod_runner.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "ply.h"
#include "thumbs.h"
#include "utils.h"

// SPZ (vendored, MIT) - native Gaussian Splat encoder/decoder.
#include "load-spz.h"
#include "splat-types.h"

namespace ddb
{
    namespace
    {
        // Recognized Gaussian Splat source format, derived from the file extension.
        enum class SplatFormat
        {
            Unknown,
            Ply,    // 3DGS PLY (INRIA layout), decoded by SPZ
            Splat,  // antimatter15 binary .splat (32 bytes/primitive, SH degree 0)
            Spz,    // canonical compressed format (copied as-is)
            Ksplat  // mkkellogg format (requires the optional external enhancer)
        };

        SplatFormat formatOf(const std::string &filename)
        {
            io::Path p(filename);
            if (p.checkExtension({"ply"}))
                return SplatFormat::Ply;
            if (p.checkExtension({"splat"}))
                return SplatFormat::Splat;
            if (p.checkExtension({"spz"}))
                return SplatFormat::Spz;
            if (p.checkExtension({"ksplat"}))
                return SplatFormat::Ksplat;
            return SplatFormat::Unknown;
        }

        // SPZ file-format version written for the delivery artifact. The web viewer (Spark)
        // only understands the gzip-based SPZ versions (1-3); version 4 switched to a ZSTD
        // per-stream container ("NGSP") that Spark cannot decode (it reports "Invalid gzip
        // header"). Version 3 keeps the same smallest-three quaternion precision as v4, so
        // visual quality is unchanged - only the compression container differs (gzip vs zstd).
        constexpr uint32_t kDeliverySpzVersion = 3;

        // True when the file begins with the gzip magic (0x1F 0x8B), i.e. a legacy SPZ v1-3
        // that the viewer can already read. NGSP/ZSTD (v4) files start with "NGSP" instead.
        bool isGzipSpz(const fs::path &path)
        {
            std::ifstream in(path.string(), std::ios::binary);
            if (!in.good())
                return false;
            uint8_t magic[2] = {0};
            in.read(reinterpret_cast<char *>(magic), sizeof(magic));
            return in.gcount() == 2 && magic[0] == 0x1F && magic[1] == 0x8B;
        }

        // Inverse sigmoid (logit) with clamping, used to store opacity the way SPZ expects.
        float inverseSigmoid(float p)
        {
            constexpr float eps = 1e-6f;
            p = std::min(std::max(p, eps), 1.0f - eps);
            return std::log(p / (1.0f - p));
        }

        // Parse the DDB_GSPLAT_SH_BITS environment override.
        // Accepts one or two unsigned integers separated by a comma:
        //   "5"   -> sets sh1Bits=5 (shRestBits unchanged)
        //   "5,4" -> sets sh1Bits=5, shRestBits=4
        // Each value must be in [1,8]. Falls back to SPZ defaults when unset or malformed.
        void applyShBitsFromEnv(spz::PackOptions &opts)
        {
            const char *env = std::getenv("DDB_GSPLAT_SH_BITS");
            if (env == nullptr || env[0] == '\0')
                return;

            const auto parts = utils::split(std::string(env), ",");
            try
            {
                if (!parts.empty() && !parts[0].empty())
                {
                    const int v = std::stoi(parts[0]);
                    if (v >= 1 && v <= 8)
                        opts.sh1Bits = static_cast<uint8_t>(v);
                }
                if (parts.size() >= 2 && !parts[1].empty())
                {
                    const int v = std::stoi(parts[1]);
                    if (v >= 1 && v <= 8)
                        opts.shRestBits = static_cast<uint8_t>(v);
                }
            }
            catch (const std::exception &e)
            {
                LOGD << "Ignoring malformed DDB_GSPLAT_SH_BITS='" << env << "': " << e.what();
            }
        }

        // Loads the delivery .spz and writes a tiny bounds.json sidecar with the local-space
        // axis-aligned bounding box of the splat centers: {"min":[x,y,z],"max":[x,y,z]}.
        // The viewer reads this to frame the camera deterministically (essential for paged .rad
        // playback, where no splats are resident at init time). Best-effort: never throws.
        void writeSplatBounds(const fs::path &spzPath, const fs::path &outFile)
        {
            try
            {
                spz::UnpackOptions unpack;
                spz::GaussianCloud cloud = spz::loadSpz(spzPath.string(), unpack);
                const size_t n = static_cast<size_t>(std::max(cloud.numPoints, 0));
                if (n == 0 || cloud.positions.size() < n * 3)
                {
                    LOGD << "Skipping bounds.json: no splat positions in " << spzPath.string();
                    return;
                }

                double mn[3] = {std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::max()};
                double mx[3] = {std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::lowest()};
                for (size_t i = 0; i < n; ++i)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        const double v = static_cast<double>(cloud.positions[i * 3 + k]);
                        mn[k] = std::min(mn[k], v);
                        mx[k] = std::max(mx[k], v);
                    }
                }

                json j;
                j["min"] = {mn[0], mn[1], mn[2]};
                j["max"] = {mx[0], mx[1], mx[2]};

                std::ofstream out(outFile.string(), std::ios::binary);
                if (!out.good())
                {
                    LOGD << "Could not open bounds.json for writing: " << outFile.string();
                    return;
                }
                out << j.dump();
            }
            catch (const std::exception &e)
            {
                LOGD << "Skipping bounds.json (" << e.what() << ")";
            }
        }

        // Decode an antimatter15 .splat file into an SPZ GaussianCloud.
        //
        // Layout (32 bytes per primitive):
        //   [0..11]  position    : 3 x float32
        //   [12..23] scale       : 3 x float32 (linear)
        //   [24..27] color       : 4 x uint8 (r, g, b, a)
        //   [28..31] rotation    : 4 x uint8, (w, x, y, z) each mapped as (b - 128) / 128
        //
        // Produces a cloud consistent with SPZ's PLY convention (scales as log, opacity as
        // logit, color as the SH DC coefficient, rotations stored [x, y, z, w]).
        spz::GaussianCloud loadSplatBinary(const std::string &filename)
        {
            std::ifstream in(filename, std::ios::binary | std::ios::ate);
            if (!in.good())
                throw FSException("Cannot open " + filename);

            const std::streamoff size = in.tellg();
            if (size <= 0 || (size % 32) != 0)
                throw AppException("Invalid .splat file (size is not a positive multiple of 32): " +
                                   filename);

            in.seekg(0, std::ios::beg);
            std::vector<uint8_t> raw(static_cast<size_t>(size));
            in.read(reinterpret_cast<char *>(raw.data()), size);
            if (!in.good())
                throw AppException("Failed to read .splat file: " + filename);

            const size_t n = static_cast<size_t>(size) / 32u;

            spz::GaussianCloud cloud;
            cloud.numPoints = static_cast<int32_t>(n);
            cloud.shDegree = 0;
            cloud.positions.resize(n * 3);
            cloud.scales.resize(n * 3);
            cloud.rotations.resize(n * 4);
            cloud.alphas.resize(n);
            cloud.colors.resize(n * 3);

            for (size_t i = 0; i < n; ++i)
            {
                const uint8_t *rec = raw.data() + i * 32u;

                float pos[3];
                float scale[3];
                std::memcpy(pos, rec + 0, sizeof(pos));
                std::memcpy(scale, rec + 12, sizeof(scale));

                const uint8_t r = rec[24];
                const uint8_t g = rec[25];
                const uint8_t b = rec[26];
                const uint8_t a = rec[27];

                const float qw = (static_cast<float>(rec[28]) - 128.0f) / 128.0f;
                const float qx = (static_cast<float>(rec[29]) - 128.0f) / 128.0f;
                const float qy = (static_cast<float>(rec[30]) - 128.0f) / 128.0f;
                const float qz = (static_cast<float>(rec[31]) - 128.0f) / 128.0f;
                float qn = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
                if (qn < 1e-9f)
                    qn = 1.0f;

                cloud.positions[i * 3 + 0] = pos[0];
                cloud.positions[i * 3 + 1] = pos[1];
                cloud.positions[i * 3 + 2] = pos[2];

                // Linear scale -> log scale (guard against non-positive values).
                cloud.scales[i * 3 + 0] = std::log(std::max(scale[0], 1e-9f));
                cloud.scales[i * 3 + 1] = std::log(std::max(scale[1], 1e-9f));
                cloud.scales[i * 3 + 2] = std::log(std::max(scale[2], 1e-9f));

                // SPZ stores rotations as [x, y, z, w].
                cloud.rotations[i * 4 + 0] = qx / qn;
                cloud.rotations[i * 4 + 1] = qy / qn;
                cloud.rotations[i * 4 + 2] = qz / qn;
                cloud.rotations[i * 4 + 3] = qw / qn;

                cloud.alphas[i] = inverseSigmoid(static_cast<float>(a) / 255.0f);

                // Recover the SH DC coefficient from the baked-in RGB color.
                cloud.colors[i * 3 + 0] =
                    (static_cast<float>(r) / 255.0f - 0.5f) / static_cast<float>(SH_C0);
                cloud.colors[i * 3 + 1] =
                    (static_cast<float>(g) / 255.0f - 0.5f) / static_cast<float>(SH_C0);
                cloud.colors[i * 3 + 2] =
                    (static_cast<float>(b) / 255.0f - 0.5f) / static_cast<float>(SH_C0);
            }

            return cloud;
        }

        // Read splat count + SH degree from a .spz file. Prefers a cheap 13-byte header
        // read for the v4 ZSTD format, falling back to a full packed load for legacy files.
        bool readSpzInfo(const std::string &filename, GaussianSplatInfo &info)
        {
            std::ifstream in(filename, std::ios::binary);
            if (!in.good())
                return false;

            uint8_t hdr[13] = {0};
            in.read(reinterpret_cast<char *>(hdr), sizeof(hdr));
            const std::streamsize got = in.gcount();
            in.close();

            // NGSP magic (v4): 'N','G','S','P' = 0x4E 0x47 0x53 0x50.
            if (got >= 13 && hdr[0] == 'N' && hdr[1] == 'G' && hdr[2] == 'S' && hdr[3] == 'P')
            {
                uint32_t numPoints = 0;
                std::memcpy(&numPoints, hdr + 8, sizeof(numPoints)); // little-endian field
                info.splatCount = numPoints;
                info.shDegree = static_cast<int>(hdr[12]);
                return true;
            }

            // Legacy gzip-compressed format (v1-3): decode the packed header.
            try
            {
                const spz::PackedGaussians packed = spz::loadSpzPacked(filename);
                if (packed.numPoints <= 0)
                    return false;
                info.splatCount = static_cast<size_t>(packed.numPoints);
                info.shDegree = packed.shDegree;
                return true;
            }
            catch (const std::exception &e)
            {
                LOGD << "Cannot read SPZ info for " << filename << ": " << e.what();
                return false;
            }
        }
    } // namespace

    json GaussianSplatInfo::toJSON() const
    {
        json j;
        j["splatCount"] = splatCount;
        j["shDegree"] = shDegree;
        return j;
    }

    json GsplatGeoref::toJSON() const
    {
        json j;
        j["srs"] = srs;
        j["offset"] = {offset[0], offset[1], offset[2]};
        if (boundsWgs84.size() == 4)
            j["boundsWgs84"] = boundsWgs84;
        return j;
    }

    bool isSpzPath(const std::string &filename)
    {
        return io::Path(filename).checkExtension({"spz"});
    }

    bool hasSplatExtension(const std::string &filename)
    {
        return io::Path(filename).checkExtension({"spz", "splat", "ksplat"});
    }

    bool looksLikeSpz(const fs::path &path)
    {
        std::ifstream in(path.string(), std::ios::binary);
        if (!in.good())
            return false;
        uint8_t magic[4] = {0};
        in.read(reinterpret_cast<char *>(magic), sizeof(magic));
        if (in.gcount() < 2)
            return false;
        // NGSP (v4) or gzip (legacy v1-3).
        const bool ngsp = (in.gcount() >= 4) && magic[0] == 'N' && magic[1] == 'G' &&
                          magic[2] == 'S' && magic[3] == 'P';
        const bool gzip = magic[0] == 0x1F && magic[1] == 0x8B;
        return ngsp || gzip;
    }

    bool looksLikeSplatBinary(const fs::path &path)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec)
            return false;
        return size > 0 && (size % 32) == 0;
    }

    bool getGaussianSplatInfo(const std::string &filename, GaussianSplatInfo &info)
    {
        info = GaussianSplatInfo();

        switch (formatOf(filename))
        {
            case SplatFormat::Ply:
            {
                PlyInfo ply;
                if (!getPlyInfo(filename, ply) || !ply.isSplat)
                    return false;
                info.splatCount = ply.vertexCount;
                info.shDegree = ply.shDegree >= 0 ? ply.shDegree : 0;
                info.dimensions = ply.dimensions;
                return true;
            }
            case SplatFormat::Splat:
            {
                std::error_code ec;
                const auto size = fs::file_size(filename, ec);
                if (ec || size == 0 || (size % 32) != 0)
                    return false;
                info.splatCount = static_cast<size_t>(size / 32);
                info.shDegree = 0;
                return true;
            }
            case SplatFormat::Spz:
                return readSpzInfo(filename, info);
            case SplatFormat::Ksplat:
            case SplatFormat::Unknown:
            default:
                // .ksplat is recognized as a type but not natively decodable here.
                return false;
        }
    }

    bool parseCoordsFile(const fs::path &coordsFile, GsplatGeoref &georef)
    {
        georef = GsplatGeoref();

        std::ifstream in(coordsFile.string());
        if (!in.good())
            return false;

        std::string line;
        // First non-empty line: SRS header.
        std::string srs;
        while (std::getline(in, line))
        {
            std::string t = line;
            utils::trim(t);
            if (!t.empty())
            {
                srs = t;
                break;
            }
        }
        if (srs.empty())
            return false;

        // Next non-empty, non-comment line: "east north [z]".
        std::string offsetLine;
        while (std::getline(in, line))
        {
            std::string t = line;
            utils::trim(t);
            if (t.empty() || t[0] == '#')
                continue;
            offsetLine = t;
            break;
        }
        if (offsetLine.empty())
            return false;

        std::array<double, 3> offset = {0.0, 0.0, 0.0};
        std::istringstream ss(offsetLine);
        if (!(ss >> offset[0] >> offset[1]))
            return false;
        ss >> offset[2]; // optional, leaves 0.0 if absent

        georef.srs = srs;
        georef.offset = offset;
        georef.valid = true;
        return true;
    }

    void writeGeoref(const GsplatGeoref &georef, const fs::path &outFile)
    {
        io::assureFolderExists(outFile.parent_path());
        std::ofstream out(outFile.string(), std::ios::binary | std::ios::trunc);
        if (!out.good())
            throw FSException("Cannot write georef sidecar " + outFile.string());
        out << georef.toJSON().dump(2);
        if (!out.good())
            throw FSException("Failed while writing " + outFile.string());
    }

    void convertToSpz(const std::string &input, const std::string &output)
    {
        const SplatFormat fmt = formatOf(input);

        if (fmt == SplatFormat::Ksplat)
            throw BuildDepMissingException(
                "Converting .ksplat requires the optional splat-transform tool", "splat-transform");

        if (fmt == SplatFormat::Unknown)
            throw InvalidArgsException("Unsupported Gaussian Splat format: " + input);

        io::assureFolderExists(fs::path(output).parent_path());

        if (fmt == SplatFormat::Spz)
        {
            if (!looksLikeSpz(input))
                throw AppException("File does not look like a valid .spz: " + input);
            // Gzip-based SPZ (v1-3) is already viewer-compatible: copy it through unchanged.
            // NGSP/ZSTD SPZ (v4) must be transcoded to v3 so the Spark viewer can decode it.
            if (isGzipSpz(input))
            {
                io::copy(input, output);
                return;
            }

            spz::UnpackOptions unpack;
            spz::GaussianCloud cloud = spz::loadSpz(input, unpack);
            if (cloud.numPoints <= 0)
                throw AppException("No splats decoded from: " + input);

            spz::PackOptions pack;
            pack.version = kDeliverySpzVersion;
            applyShBitsFromEnv(pack);
            if (!spz::saveSpz(cloud, pack, output))
                throw AppException("Failed to transcode .spz to viewer-compatible version: " + output);
            return;
        }

        spz::GaussianCloud cloud;
        if (fmt == SplatFormat::Ply)
        {
            spz::UnpackOptions unpack; // canonical (no coordinate conversion)
            cloud = spz::loadSplatFromPly(input, unpack);
            if (cloud.numPoints <= 0)
                throw AppException("Not a valid Gaussian Splat PLY (no splats decoded): " + input);
        }
        else // SplatFormat::Splat
        {
            cloud = loadSplatBinary(input);
            if (cloud.numPoints <= 0)
                throw AppException("No splats decoded from: " + input);
        }

        spz::PackOptions pack; // canonical (from = UNSPECIFIED)
        pack.version = kDeliverySpzVersion;
        applyShBitsFromEnv(pack);

        if (!spz::saveSpz(cloud, pack, output))
            throw AppException("Failed to encode .spz: " + output);
    }

    void buildGsplat(const std::string &input, const std::string &outdir)
    {
        GsplatGeoref none;
        buildGsplat(input, outdir, none);
    }

    void buildGsplat(const std::string &input, const std::string &outdir, const GsplatGeoref &georef)
    {
        io::assureFolderExists(outdir);

        const fs::path outSpz = fs::path(outdir) / GsplatFileName;
        convertToSpz(input, outSpz.string());

        // Local-space bounding box sidecar for deterministic camera framing in the viewer.
        writeSplatBounds(outSpz, fs::path(outdir) / GsplatBoundsFileName);

        // Pre-render a preview from the canonical .spz so file lists show a real thumbnail.
        // generateThumb dispatches on the .spz extension to the splat renderer. Best-effort:
        // a thumbnail failure must never fail the build.
        const fs::path outThumb = fs::path(outdir) / GsplatThumbFileName;
        try
        {
            generateThumb(outSpz, 512, outThumb, /*forceRecreate=*/true, nullptr, nullptr);
        }
        catch (const std::exception &e)
        {
            LOGD << "Skipping Gaussian Splat thumbnail (" << e.what() << ")";
        }

        // Optional level-of-detail artifact for progressive streaming of large scenes.
        // Derived from the canonical model.spz so it stays inside the build temp folder
        // (build-lod writes next to its input) and reuses the same quantization. This is
        // best-effort: when build-lod is not bundled, or fails, we keep model.spz and the
        // viewer falls back to non-streamed loading. See TBD/GaussianSplats/LOD-Support.md.
        bool radProduced = false;
        if (buildlod::isBuildLodAvailable())
        {
            const fs::path outRad = fs::path(outdir) / GsplatRadFileName;
            std::string lodError;
            if (buildlod::runBuildLod(outSpz, outRad, lodError, /*quality=*/true, GsplatRadMaxSh))
            {
                radProduced = true;
                LOGD << "Generated Gaussian Splat LOD: " << outRad.string();
            }
            else
                LOGD << "Skipping Gaussian Splat LOD (" << lodError << ")";
        }
        else
        {
            LOGD << "build-lod not available; serving Gaussian Splat without LOD streaming";
        }

        // Drop the canonical .spz once the full-SH .rad exists: the viewer streams the .rad,
        // the thumbnail is pre-rendered, and downloads serve the original upload - so keeping
        // the .spz would only duplicate storage. Keep it as the delivery artifact when no .rad
        // was produced (graceful degradation without build-lod).
        if (radProduced)
        {
            std::error_code ec;
            fs::remove(outSpz, ec);
            if (ec)
                LOGD << "Could not remove redundant " << outSpz.string() << ": " << ec.message();
        }

        if (georef.valid)
            writeGeoref(georef, fs::path(outdir) / GsplatGeorefFileName);
    }

} // namespace ddb
