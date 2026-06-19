/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "rad.h"

#include <cstring>
#include <fstream>
#include <limits>

#include <zlib.h>

#include "exceptions.h"
#include "json.h"
#include "mio.h"

namespace ddb
{
    namespace
    {
        // File and chunk magic numbers (little-endian), see vendor/spark rust/spark-lib/src/rad.rs.
        constexpr uint32_t kRadMagic = 0x30444152;      // 'RAD0'
        constexpr uint32_t kRadChunkMagic = 0x43444152; // 'RADC'

        // RAD aligns sub-sections to 8 bytes.
        inline size_t roundUp8(size_t n) { return (n + 7u) & ~static_cast<size_t>(7u); }

        inline uint32_t readU32LE(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, sizeof(v));
            return v; // DDB targets little-endian platforms only.
        }

        // IEEE-754 half -> float (matches Rust `half` crate semantics incl. inf/nan).
        float halfToFloat(uint16_t h)
        {
            const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
            const uint32_t exp = (h >> 10) & 0x1Fu;
            const uint32_t mant = h & 0x3FFu;
            uint32_t bits;
            if (exp == 0)
            {
                if (mant == 0)
                {
                    bits = sign; // +/- 0
                }
                else
                {
                    // Subnormal half -> normalized float.
                    int e = -1;
                    uint32_t m = mant;
                    do
                    {
                        m <<= 1;
                        ++e;
                    } while ((m & 0x400u) == 0);
                    m &= 0x3FFu;
                    bits = sign | ((127 - 15 - e) << 23) | (m << 13);
                }
            }
            else if (exp == 0x1F)
            {
                bits = sign | 0x7F800000u | (mant << 13); // inf / nan
            }
            else
            {
                bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
            }
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }

        // Raw DEFLATE (no zlib/gzip header) - miniz_oxide `compress_to_vec`, used by RAD ("gz").
        std::vector<uint8_t> inflateRaw(const uint8_t *data, size_t size, size_t expectedHint)
        {
            z_stream strm{};
            // windowBits = -15 selects raw deflate (no header / no checksum).
            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
                throw AppException("RAD: failed to initialize raw inflate");

            std::vector<uint8_t> out;
            out.reserve(expectedHint > 0 ? expectedHint : size * 4);
            strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
            strm.avail_in = static_cast<uInt>(size);

            uint8_t buffer[64 * 1024];
            int ret = Z_OK;
            do
            {
                strm.next_out = buffer;
                strm.avail_out = sizeof(buffer);
                ret = inflate(&strm, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END)
                {
                    inflateEnd(&strm);
                    throw AppException("RAD: raw inflate failed (zlib code " + std::to_string(ret) + ")");
                }
                out.insert(out.end(), buffer, buffer + (sizeof(buffer) - strm.avail_out));
            } while (ret != Z_STREAM_END);

            inflateEnd(&strm);
            return out;
        }

        // ---- Property decoders. All produce interleaved output [splat * dims + dim]. ----
        // Layouts mirror vendor/spark rust/spark-lib/src/rad.rs decode_* exactly.

        std::vector<float> decodeF32(const std::vector<uint8_t> &d, int dims, size_t count)
        {
            std::vector<float> out(dims * count);
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const size_t byteOff = (static_cast<size_t>(dim) * count + i) * 4u;
                    float v;
                    std::memcpy(&v, d.data() + byteOff, 4);
                    out[i * dims + dim] = v;
                }
            return out;
        }

        std::vector<float> decodeF16(const std::vector<uint8_t> &d, int dims, size_t count)
        {
            std::vector<float> out(dims * count);
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const size_t byteOff = (static_cast<size_t>(dim) * count + i) * 2u;
                    uint16_t h;
                    std::memcpy(&h, d.data() + byteOff, 2);
                    out[i * dims + dim] = halfToFloat(h);
                }
            return out;
        }

        std::vector<float> decodeF32LeBytes(const std::vector<uint8_t> &d, int dims, size_t count)
        {
            std::vector<float> out(dims * count);
            const size_t stride = count * static_cast<size_t>(dims);
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const size_t base = count * static_cast<size_t>(dim) + i;
                    uint8_t bytes[4] = {d[base], d[base + stride], d[base + stride * 2], d[base + stride * 3]};
                    float v;
                    std::memcpy(&v, bytes, 4);
                    out[i * dims + dim] = v;
                }
            return out;
        }

        std::vector<float> decodeF16LeBytes(const std::vector<uint8_t> &d, int dims, size_t count)
        {
            std::vector<float> out(dims * count);
            const size_t stride = count * static_cast<size_t>(dims);
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const size_t base = count * static_cast<size_t>(dim) + i;
                    const uint16_t h = static_cast<uint16_t>(d[base]) |
                                       (static_cast<uint16_t>(d[base + stride]) << 8);
                    out[i * dims + dim] = halfToFloat(h);
                }
            return out;
        }

        std::vector<float> decodeR8(const std::vector<uint8_t> &d, int dims, size_t count, float mn, float mx)
        {
            std::vector<float> out(dims * count);
            const float span = mx - mn;
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const size_t byteOff = static_cast<size_t>(dim) * count + i;
                    out[i * dims + dim] = (static_cast<float>(d[byteOff]) / 255.0f) * span + mn;
                }
            return out;
        }

        std::vector<float> decodeR8Delta(const std::vector<uint8_t> &d, int dims, size_t count, float mn, float mx)
        {
            std::vector<float> out(dims * count);
            const float span = mx - mn;
            std::vector<uint8_t> last(dims, 0);
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const size_t byteOff = static_cast<size_t>(dim) * count + i;
                    const uint8_t v = static_cast<uint8_t>(last[dim] + d[byteOff]); // wrapping add
                    last[dim] = v;
                    out[i * dims + dim] = (static_cast<float>(v) / 255.0f) * span + mn;
                }
            return out;
        }

        std::vector<float> decodeProperty(const std::vector<uint8_t> &raw, const std::string &encoding,
                                          int dims, size_t count, float mn, float mx)
        {
            const size_t need = static_cast<size_t>(dims) * count;
            auto checkSize = [&](size_t perElem)
            {
                if (raw.size() < need * perElem)
                    throw AppException("RAD: property payload too small for encoding " + encoding);
            };

            if (encoding == "f32_lebytes") { checkSize(4); return decodeF32LeBytes(raw, dims, count); }
            if (encoding == "f16_lebytes") { checkSize(2); return decodeF16LeBytes(raw, dims, count); }
            if (encoding == "f32") { checkSize(4); return decodeF32(raw, dims, count); }
            if (encoding == "f16") { checkSize(2); return decodeF16(raw, dims, count); }
            if (encoding == "r8") { checkSize(1); return decodeR8(raw, dims, count, mn, mx); }
            if (encoding == "r8_delta") { checkSize(1); return decodeR8Delta(raw, dims, count, mn, mx); }

            throw AppException("RAD: unsupported encoding for preview/bounds: " + encoding);
        }

        struct RadChunkRef
        {
            uint64_t fileOffset = 0; // absolute byte offset of the chunk in the file
            uint64_t bytes = 0;      // total chunk size
        };

        struct RadHeader
        {
            std::vector<RadChunkRef> chunks;
        };

        // Reads and validates the RAD file header, returning the absolute file offsets of all chunks.
        RadHeader readHeader(std::ifstream &in, const std::string &path)
        {
            uint8_t hb[8];
            in.read(reinterpret_cast<char *>(hb), 8);
            if (!in.good())
                throw AppException("RAD: cannot read header of " + path);
            if (readU32LE(hb) != kRadMagic)
                throw AppException("RAD: bad magic (not a .rad file): " + path);

            const uint32_t metaLen = readU32LE(hb + 4);
            std::string metaStr(metaLen, '\0');
            in.read(metaStr.data(), metaLen);
            if (!in.good())
                throw AppException("RAD: truncated header in " + path);

            json meta;
            try
            {
                meta = json::parse(metaStr);
            }
            catch (const std::exception &e)
            {
                throw AppException(std::string("RAD: invalid header JSON: ") + e.what());
            }

            const uint64_t chunksStart = 8u + roundUp8(metaLen);
            RadHeader header;
            if (!meta.contains("chunks") || !meta["chunks"].is_array())
                throw AppException("RAD: header has no chunks array");

            for (const auto &c : meta["chunks"])
            {
                // Single-file RADs use in-file offsets; chunked RADs reference external .radc
                // files which this minimal reader does not support.
                if (c.contains("filename") && !c["filename"].is_null())
                    throw AppException("RAD: chunked (.radc) files are not supported by this reader");

                RadChunkRef ref;
                ref.fileOffset = chunksStart + c.value("offset", static_cast<uint64_t>(0));
                ref.bytes = c.value("bytes", static_cast<uint64_t>(0));
                header.chunks.push_back(ref);
            }
            return header;
        }

        struct DecodedChunk
        {
            size_t count = 0;
            std::vector<float> centers;   // count * 3
            std::vector<float> colors;    // count * 3 (empty when not requested / absent)
            std::vector<float> opacities; // count     (empty when not requested / absent)
        };

        // Decodes one chunk. `centers` is always produced; colours/opacities only when wantColor.
        DecodedChunk decodeChunk(std::ifstream &in, const RadChunkRef &ref, const std::string &path,
                                 bool wantColor)
        {
            if (ref.bytes < 16)
                throw AppException("RAD: chunk too small in " + path);

            std::vector<uint8_t> buf(ref.bytes);
            in.seekg(static_cast<std::streamoff>(ref.fileOffset), std::ios::beg);
            in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(ref.bytes));
            if (!in.good())
                throw AppException("RAD: cannot read chunk payload in " + path);

            if (readU32LE(buf.data()) != kRadChunkMagic)
                throw AppException("RAD: bad chunk magic in " + path);
            const uint32_t cmetaLen = readU32LE(buf.data() + 4);
            const size_t cmetaStart = 8;
            if (cmetaStart + cmetaLen > buf.size())
                throw AppException("RAD: chunk meta out of range in " + path);

            json cmeta;
            try
            {
                cmeta = json::parse(std::string(reinterpret_cast<const char *>(buf.data() + cmetaStart), cmetaLen));
            }
            catch (const std::exception &e)
            {
                throw AppException(std::string("RAD: invalid chunk JSON: ") + e.what());
            }

            DecodedChunk out;
            out.count = cmeta.value("count", static_cast<size_t>(0));
            if (out.count == 0)
                return out;

            const size_t payloadStart = 8u + roundUp8(cmetaLen) + 8u; // + u64 payloadBytes field

            if (!cmeta.contains("properties") || !cmeta["properties"].is_array())
                throw AppException("RAD: chunk has no properties array");

            for (const auto &p : cmeta["properties"])
            {
                const std::string name = p.value("property", std::string());
                const bool isCenter = name == "center";
                const bool isRgb = name == "rgb";
                const bool isAlpha = name == "alpha";
                if (!isCenter && !(wantColor && (isRgb || isAlpha)))
                    continue;

                const std::string encoding = p.value("encoding", std::string());
                const uint64_t offset = p.value("offset", static_cast<uint64_t>(0));
                const uint64_t bytes = p.value("bytes", static_cast<uint64_t>(0));
                const float mn = p.value("min", 0.0f);
                const float mx = p.value("max", 1.0f);
                const std::string compression = p.value("compression", std::string());

                const size_t dataStart = payloadStart + offset;
                if (dataStart + bytes > buf.size())
                    throw AppException("RAD: property '" + name + "' out of range in " + path);

                std::vector<uint8_t> raw;
                if (compression == "gz")
                {
                    const int dims = isCenter ? 3 : (isRgb ? 3 : 1);
                    raw = inflateRaw(buf.data() + dataStart, bytes, static_cast<size_t>(dims) * out.count * 4);
                }
                else
                {
                    raw.assign(buf.begin() + dataStart, buf.begin() + dataStart + bytes);
                }

                if (isCenter)
                    out.centers = decodeProperty(raw, encoding, 3, out.count, mn, mx);
                else if (isRgb)
                    out.colors = decodeProperty(raw, encoding, 3, out.count, mn, mx);
                else if (isAlpha)
                    out.opacities = decodeProperty(raw, encoding, 1, out.count, mn, mx);
            }

            if (out.centers.size() < out.count * 3)
                throw AppException("RAD: chunk missing center data in " + path);
            return out;
        }

    } // namespace

    bool isRadPath(const std::string &filename)
    {
        return io::Path(filename).checkExtension({"rad"});
    }

    RadCoarseSplats readRadCoarseSplats(const fs::path &radPath, int maxChunks)
    {
        std::ifstream in(radPath.string(), std::ios::binary);
        if (!in.good())
            throw AppException("RAD: cannot open " + radPath.string());

        const RadHeader header = readHeader(in, radPath.string());

        RadCoarseSplats result;
        const int limit = maxChunks <= 0 ? static_cast<int>(header.chunks.size())
                                         : std::min<int>(maxChunks, static_cast<int>(header.chunks.size()));
        for (int i = 0; i < limit; ++i)
        {
            DecodedChunk chunk = decodeChunk(in, header.chunks[i], radPath.string(), /*wantColor=*/true);
            if (chunk.count == 0)
                continue;

            const size_t baseIdx = result.count;
            result.count += chunk.count;
            result.positions.insert(result.positions.end(), chunk.centers.begin(), chunk.centers.end());

            // Default colours to mid-grey and full opacity if the chunk lacked them, so the
            // preview still renders geometry.
            if (chunk.colors.size() == chunk.count * 3)
                result.colors.insert(result.colors.end(), chunk.colors.begin(), chunk.colors.end());
            else
                result.colors.resize(result.positions.size(), 0.5f);

            if (chunk.opacities.size() == chunk.count)
                result.opacities.insert(result.opacities.end(), chunk.opacities.begin(), chunk.opacities.end());
            else
                result.opacities.resize(result.count, 1.0f);

            (void)baseIdx;
        }

        if (result.count == 0)
            throw AppException("RAD: no splats decoded from " + radPath.string());
        return result;
    }

    bool computeRadBounds(const fs::path &radPath, std::array<double, 3> &outMin,
                          std::array<double, 3> &outMax)
    {
        std::ifstream in(radPath.string(), std::ios::binary);
        if (!in.good())
            throw AppException("RAD: cannot open " + radPath.string());

        const RadHeader header = readHeader(in, radPath.string());

        outMin = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max()};
        outMax = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                  std::numeric_limits<double>::lowest()};

        bool any = false;
        for (const auto &ref : header.chunks)
        {
            DecodedChunk chunk = decodeChunk(in, ref, radPath.string(), /*wantColor=*/false);
            for (size_t i = 0; i < chunk.count; ++i)
            {
                for (int k = 0; k < 3; ++k)
                {
                    const double v = static_cast<double>(chunk.centers[i * 3 + k]);
                    if (v < outMin[k]) outMin[k] = v;
                    if (v > outMax[k]) outMax[k] = v;
                }
            }
            if (chunk.count > 0)
                any = true;
        }
        return any;
    }

} // namespace ddb
