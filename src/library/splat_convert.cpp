/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "splat_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

#include "cancellation.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "splat_format.h"
#include "splat_math.h"
#include "stringutil.h"

// SPZ (vendored, MIT) - native Gaussian Splat encoder/decoder.
#include "load-spz.h"
#include "splat-types.h"

namespace ddb
{
    namespace
    {
        // SPZ file-format version written for the delivery artifact. The web viewer (Spark)
        // only understands the gzip-based SPZ versions (1-3); version 4 switched to a ZSTD
        // per-stream container ("NGSP") that Spark cannot decode (it reports "Invalid gzip
        // header"). Version 3 keeps the same smallest-three quaternion precision as v4, so
        // visual quality is unchanged - only the compression container differs (gzip vs zstd).
        constexpr uint32_t kDeliverySpzVersion = 3;

        // SPZ's file name overloads open a std::string with the narrow code page on Windows
        // (mio.h, "Path encoding"): the file is written here, through its buffer overload and the
        // atomic writer, so a reader (and the file a conversion replaces) sees the previous file or
        // the whole new one, never a part. Same result as spz::saveSpz(cloud, options, filename).
        // False when SPZ cannot encode the cloud; throws FSException when the file cannot be written.
        bool saveSpzFile(const spz::GaussianCloud &cloud, const spz::PackOptions &options, const fs::path &file)
        {
            std::vector<uint8_t> data;
            if (!spz::saveSpz(cloud, options, &data))
                return false;
            io::writeFileAtomic(file, std::string_view(reinterpret_cast<const char *>(data.data()), data.size()),
                                ".spz file");
            return true;
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

            const auto parts = str::split(std::string(env), ",");
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

        // The options of the delivery .spz: version 3 and the quantization of the environment.
        spz::PackOptions deliveryPackOptions()
        {
            spz::PackOptions pack; // canonical (from = UNSPECIFIED)
            pack.version = kDeliverySpzVersion;
            applyShBitsFromEnv(pack);
            return pack;
        }

        // Decode an antimatter15 .splat file into an SPZ GaussianCloud.
        //
        // Layout (SplatRecordSize = 32 bytes per primitive):
        //   [0..11]  position    : 3 x float32
        //   [12..23] scale       : 3 x float32 (linear)
        //   [24..27] color       : 4 x uint8 (r, g, b, a)
        //   [28..31] rotation    : 4 x uint8, (w, x, y, z) each mapped as (b - 128) / 128
        //
        // Produces a cloud consistent with SPZ's PLY convention (scales as log, opacity as
        // logit, color as the SH DC coefficient, rotations stored [x, y, z, w]).
        spz::GaussianCloud loadSplatBinary(const std::string &filename)
        {
            std::ifstream in(io::fromUtf8(filename), std::ios::binary | std::ios::ate);
            if (!in.good())
                throw FSException("Cannot open " + filename);

            const std::streamoff size = in.tellg();
            if (size <= 0 || (static_cast<uint64_t>(size) % SplatRecordSize) != 0)
                throw AppException("Invalid .splat file (size is not a positive multiple of " +
                                   std::to_string(SplatRecordSize) + "): " + filename);

            // Before the file is read: a count SPZ cannot hold would only be cut short afterwards
            const size_t n = static_cast<size_t>(static_cast<uint64_t>(size) / SplatRecordSize);
            const int32_t numPoints = spzPointCount(n, filename);

            in.seekg(0, std::ios::beg);
            std::vector<uint8_t> raw(static_cast<size_t>(size));
            in.read(reinterpret_cast<char *>(raw.data()), size);
            if (!in.good())
                throw AppException("Failed to read .splat file: " + filename);

            spz::GaussianCloud cloud;
            cloud.numPoints = numPoints;
            cloud.shDegree = 0;
            cloud.positions.resize(n * 3);
            cloud.scales.resize(n * 3);
            cloud.rotations.resize(n * 4);
            cloud.alphas.resize(n);
            cloud.colors.resize(n * 3);

            for (size_t i = 0; i < n; ++i)
            {
                const uint8_t *rec = raw.data() + i * SplatRecordSize;

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

                cloud.alphas[i] = splat_math::logit(static_cast<float>(a) / 255.0f);

                // Recover the SH DC coefficient from the baked-in RGB color.
                cloud.colors[i * 3 + 0] = splat_math::colorToDc(static_cast<float>(r) / 255.0f);
                cloud.colors[i * 3 + 1] = splat_math::colorToDc(static_cast<float>(g) / 255.0f);
                cloud.colors[i * 3 + 2] = splat_math::colorToDc(static_cast<float>(b) / 255.0f);
            }

            return cloud;
        }
    } // namespace

    int32_t spzPointCount(uint64_t splats, const std::string &source)
    {
        constexpr uint64_t maxSplats = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
        if (splats > maxSplats)
            throw AppException(source + " has " + std::to_string(splats) + " splats, more than the " +
                               std::to_string(maxSplats) + " a .spz file can hold");
        return static_cast<int32_t>(splats);
    }

    void transcodeSpzToV3(const fs::path &input, const fs::path &output)
    {
        // SPZ has no streaming reader: the file is read, and its splats decoded, whole in memory
        spz::UnpackOptions unpack;
        spz::GaussianCloud cloud = spz::loadSpz(io::readAllBytes(input), unpack);
        if (cloud.numPoints <= 0)
            throw AppException("No splats decoded from: " + io::toUtf8(input));

        if (!saveSpzFile(cloud, deliveryPackOptions(), output))
            throw AppException("Failed to transcode .spz to viewer-compatible version: " + io::toUtf8(output));
    }

    void assureConvertibleToSpz(const std::string &input)
    {
        if (!splatAccepts(io::fromUtf8(input), SplatStage::Convert))
            throw InvalidArgsException("Cannot convert " + input + " to .spz: only " + splatExtensionsText(SplatStage::Convert) +
                                       " files are converted");
    }

    void convertToSpz(const std::string &input, const std::string &output)
    {
        assureConvertibleToSpz(input);
        const SplatFormat fmt = splatFormatOf(io::fromUtf8(input));

        // The output replaces what is at its path: the input must not be it ("x.spz" converted
        // to the default "x.spz"), or the conversion would overwrite the file it converts
        io::assureDistinctOutput(io::fromUtf8(input), io::fromUtf8(output));

        const fs::path parentDir = io::fromUtf8(output).parent_path();
        if (!parentDir.empty())
            io::assureFolderExists(parentDir);

        if (fmt == SplatFormat::Spz)
        {
            const SpzContainer container = sniffSpzContainer(io::fromUtf8(input));
            if (container == SpzContainer::None)
                throw AppException("File does not look like a valid .spz: " + input);
            // Gzip-based SPZ (v1-3) is already viewer-compatible: copy it through unchanged.
            // NGSP/ZSTD SPZ (v4) must be transcoded to v3 so the Spark viewer can decode it.
            if (container == SpzContainer::Gzip)
            {
                const auto bytes = io::readAllBytes(io::fromUtf8(input));
                io::writeFileAtomic(io::fromUtf8(output),
                                    std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
                                    ".spz file");
                return;
            }

            transcodeSpzToV3(io::fromUtf8(input), io::fromUtf8(output));
            return;
        }

        spz::GaussianCloud cloud;
        if (fmt == SplatFormat::Ply)
        {
            spz::UnpackOptions unpack; // canonical (no coordinate conversion)
            // SPZ has no buffer overload for PLY: on Windows its narrow file name follows the
            // CRT locale (UTF-8 after DDBRegisterProcess, mio.h "Path encoding")
            cloud = spz::loadSplatFromPly(input, unpack);
            if (cloud.numPoints <= 0)
                throw AppException("Not a valid Gaussian Splat PLY (no splats decoded): " + input);
        }
        else if (fmt == SplatFormat::Splat)
        {
            cloud = loadSplatBinary(input);
            if (cloud.numPoints <= 0)
                throw AppException("No splats decoded from: " + input);
        }
        else
        {
            // A format the table accepts for conversion needs its decoder here
            throw AppException("No decoder for " + input);
        }

        // Decoding is the long read of a big file: a canceled call stops before it encodes and writes
        cancel::throwIfCanceled();
        if (!saveSpzFile(cloud, deliveryPackOptions(), io::fromUtf8(output)))
            throw AppException("Failed to encode .spz: " + output);
    }

} // namespace ddb
