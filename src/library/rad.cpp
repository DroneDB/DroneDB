/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "rad.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include <zlib.h>

#include "exceptions.h"
#include "json.h"
#include "mio.h"

// The f32 properties are read as the bytes are on disk, little-endian
static_assert(std::endian::native == std::endian::little, "the RAD reader assumes a little-endian host");

namespace ddb
{
    namespace
    {
        // File and chunk magic numbers (little-endian), see vendor/spark rust/spark-lib/src/rad.rs.
        constexpr uint32_t kRadMagic = 0x30444152;      // 'RAD0'
        constexpr uint32_t kRadChunkMagic = 0x43444152; // 'RADC'

        // RAD aligns sub-sections to 8 bytes.
        inline uint64_t roundUp8(uint64_t n) { return (n + 7u) & ~static_cast<uint64_t>(7u); }

        // build-lod writes chunks of 65536 splats. A chunk that declares more than 64 times that is
        // not one: the count sizes the buffers of the decode, and it comes from the file.
        constexpr size_t kMaxSplatsPerChunk = size_t(1) << 22;

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
        // The output is at most `maxOutput` bytes (the size the chunk declares for the property):
        // a stream that inflates to more is refused instead of filling the memory.
        std::vector<uint8_t> inflateRaw(const uint8_t *data, size_t size, size_t maxOutput)
        {
            if (size > std::numeric_limits<uInt>::max())
                throw AppException("RAD: compressed property of " + std::to_string(size) + " bytes is too large");

            z_stream strm{};
            // windowBits = -15 selects raw deflate (no header / no checksum).
            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
                throw AppException("RAD: failed to initialize raw inflate");

            std::vector<uint8_t> out;
            out.reserve(std::min(maxOutput, size * 4));
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
                const size_t produced = sizeof(buffer) - strm.avail_out;
                if (produced > maxOutput - out.size())
                {
                    inflateEnd(&strm);
                    throw AppException("RAD: compressed property inflates to more than its declared " +
                                       std::to_string(maxOutput) + " bytes");
                }
                out.insert(out.end(), buffer, buffer + produced);
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

        // Scales are stored as natural logarithms; the encoded 0 of ln_0r8 is a scale of exactly 0
        // and 1..255 span [min, max] of the logarithm (spark-lib splat_encode.rs decode_scale8).
        std::vector<float> decodeLn0R8(const std::vector<uint8_t> &d, int dims, size_t count, float mn, float mx)
        {
            std::vector<float> out(dims * count);
            const float step = (mx - mn) / 254.0f;
            for (size_t i = 0; i < count; ++i)
                for (int dim = 0; dim < dims; ++dim)
                {
                    const uint8_t v = d[static_cast<size_t>(dim) * count + i];
                    out[i * dims + dim] = v == 0 ? 0.0f : std::exp(mn + static_cast<float>(v - 1) * step);
                }
            return out;
        }

        std::vector<float> decodeLnF16(const std::vector<uint8_t> &d, int dims, size_t count)
        {
            std::vector<float> out = decodeF16(d, dims, count);
            for (float &v : out)
                v = std::exp(v);
            return out;
        }

        // A rotation as 3 bytes: an octahedral-mapped axis (2 bytes) and the angle (1 byte), see
        // spark-lib splat_encode.rs decode_quat_oct888. One quaternion (x, y, z, w) per splat.
        std::vector<float> decodeQuatOct88R8(const std::vector<uint8_t> &d, size_t count)
        {
            constexpr float kPi = 3.14159265358979323846f;
            std::vector<float> out(4 * count);
            for (size_t i = 0; i < count; ++i)
            {
                float x = static_cast<float>(d[i * 3]) / 255.0f * 2.0f - 1.0f;
                float y = static_cast<float>(d[i * 3 + 1]) / 255.0f * 2.0f - 1.0f;
                const float z = 1.0f - std::abs(x) - std::abs(y);
                const float t = std::max(-z, 0.0f);
                x = x >= 0.0f ? x - t : x + t;
                y = y >= 0.0f ? y - t : y + t;
                const float length = std::sqrt(x * x + y * y + z * z);
                const float halfTheta = static_cast<float>(d[i * 3 + 2]) / 255.0f * 0.5f * kPi;
                const float s = std::sin(halfTheta) / length;
                out[i * 4] = x * s;
                out[i * 4 + 1] = y * s;
                out[i * 4 + 2] = z * s;
                out[i * 4 + 3] = std::cos(halfTheta);
            }
            return out;
        }

        // The (x, y, z) of the f32 and f16 encodings of an orientation, with w completing the unit
        // quaternion
        std::vector<float> quatFromXyz(const std::vector<float> &xyz, size_t count)
        {
            std::vector<float> out(4 * count);
            for (size_t i = 0; i < count; ++i)
            {
                const float x = xyz[i * 3], y = xyz[i * 3 + 1], z = xyz[i * 3 + 2];
                out[i * 4] = x;
                out[i * 4 + 1] = y;
                out[i * 4 + 2] = z;
                out[i * 4 + 3] = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y - z * z));
            }
            return out;
        }

        std::vector<float> decodeOrientation(const std::vector<uint8_t> &raw, const std::string &encoding,
                                             size_t count)
        {
            if (encoding == "oct88r8")
            {
                if (raw.size() < 3 * count)
                    throw AppException("RAD: property payload too small for encoding " + encoding);
                return decodeQuatOct88R8(raw, count);
            }
            if (encoding == "f32" || encoding == "f16")
            {
                if (raw.size() < 3 * count * (encoding == "f32" ? 4u : 2u))
                    throw AppException("RAD: property payload too small for encoding " + encoding);
                return quatFromXyz(encoding == "f32" ? decodeF32(raw, 3, count) : decodeF16(raw, 3, count), count);
            }
            throw AppException("RAD: unsupported orientation encoding: " + encoding);
        }

        // The min and max a property declares for its range encodings
        struct PropertyRange
        {
            float min = 0.0f;
            float max = 1.0f;
            bool declared = false;
        };

        std::vector<float> decodeProperty(const std::vector<uint8_t> &raw, const std::string &encoding,
                                          int dims, size_t count, const PropertyRange &range)
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
            if (encoding == "r8") { checkSize(1); return decodeR8(raw, dims, count, range.min, range.max); }
            if (encoding == "r8_delta") { checkSize(1); return decodeR8Delta(raw, dims, count, range.min, range.max); }
            if (encoding == "ln_f16") { checkSize(2); return decodeLnF16(raw, dims, count); }
            if (encoding == "ln_0r8")
            {
                // The logarithms it spans are not guessable: a property without them is not read
                if (!range.declared)
                    throw AppException("RAD: property of encoding ln_0r8 has no min and max");
                checkSize(1);
                return decodeLn0R8(raw, dims, count, range.min, range.max);
            }

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
            uint64_t fileSize = 0; // the chunks are read from the file: what they declare is checked against it
        };

        // The unsigned integer `key` of a JSON object (0 when it is absent). The numbers of a RAD
        // size reads and buffers, so a negative, fractional or non numeric one is refused rather
        // than converted.
        uint64_t unsignedField(const json &object, const char *key, const std::string &path)
        {
            const auto it = object.find(key);
            if (it == object.end() || it->is_null())
                return 0;
            if (!it->is_number_unsigned() && !(it->is_number_integer() && it->get<int64_t>() >= 0))
                throw AppException(std::string("RAD: '") + key + "' is not an unsigned integer in " + path);
            return it->get<uint64_t>();
        }

        // Whether [offset, offset + bytes) lies inside `limit` bytes; written so that the sum is never formed
        bool fitsWithin(uint64_t offset, uint64_t bytes, uint64_t limit)
        {
            return offset <= limit && bytes <= limit - offset;
        }

        // Reads and validates the RAD file header, returning the absolute file offsets of all chunks.
        RadHeader readHeader(std::ifstream &in, const std::string &path)
        {
            in.seekg(0, std::ios::end);
            const std::streamoff end = in.tellg();
            in.seekg(0, std::ios::beg);
            if (end < 0 || !in.good())
                throw AppException("RAD: cannot read " + path);
            RadHeader header;
            header.fileSize = static_cast<uint64_t>(end);

            uint8_t hb[8];
            in.read(reinterpret_cast<char *>(hb), 8);
            if (!in.good())
                throw AppException("RAD: cannot read header of " + path);
            if (readU32LE(hb) != kRadMagic)
                throw AppException("RAD: bad magic (not a .rad file): " + path);

            const uint32_t metaLen = readU32LE(hb + 4);
            if (!fitsWithin(8, metaLen, header.fileSize))
                throw AppException("RAD: the header declares " + std::to_string(metaLen) +
                                   " bytes of metadata but " + path + " holds " + std::to_string(header.fileSize) +
                                   " bytes");
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
            if (!meta.is_object() || !meta.contains("chunks") || !meta["chunks"].is_array())
                throw AppException("RAD: header has no chunks array");

            for (const auto &c : meta["chunks"])
            {
                if (!c.is_object())
                    throw AppException("RAD: a chunk entry of the header is not an object in " + path);
                // Single-file RADs use in-file offsets; chunked RADs reference external .radc
                // files which this minimal reader does not support.
                if (c.contains("filename") && !c["filename"].is_null())
                    throw AppException("RAD: chunked (.radc) files are not supported by this reader");

                const uint64_t offset = unsignedField(c, "offset", path);
                RadChunkRef ref;
                ref.bytes = unsignedField(c, "bytes", path);
                // The chunk is read from the file: it has to lie in it
                if (!fitsWithin(chunksStart, offset, header.fileSize) ||
                    !fitsWithin(chunksStart + offset, ref.bytes, header.fileSize))
                    throw AppException("RAD: the chunk of " + std::to_string(ref.bytes) + " bytes at offset " +
                                       std::to_string(offset) + " is beyond the end of " + path + " (" +
                                       std::to_string(header.fileSize) + " bytes)");
                ref.fileOffset = chunksStart + offset;
                header.chunks.push_back(ref);
            }
            return header;
        }

        // What a chunk is read for: the centres are always decoded
        struct RadAttributes
        {
            bool appearance = false; // rgb and alpha
            bool shape = false;      // scales and orientation
        };

        struct DecodedChunk
        {
            size_t count = 0;
            std::vector<float> centers;   // count * 3
            std::vector<float> colors;    // count * 3 (empty when not requested / absent)
            std::vector<float> opacities; // count     (empty when not requested / absent)
            std::vector<float> scales;    // count * 3 (empty when not requested / absent)
            std::vector<float> rotations; // count * 4, x y z w (empty when not requested / absent)
        };

        // Decodes one chunk: its centres, and the attributes `wanted` names.
        DecodedChunk decodeChunk(std::ifstream &in, const RadHeader &header, const RadChunkRef &ref,
                                 const std::string &path, const RadAttributes &wanted)
        {
            if (ref.bytes < 16)
                throw AppException("RAD: chunk too small in " + path);
            // readHeader checked it; the buffer is allocated from what the file declares
            if (!fitsWithin(ref.fileOffset, ref.bytes, header.fileSize))
                throw AppException("RAD: chunk is beyond the end of " + path);

            std::vector<uint8_t> buf(static_cast<size_t>(ref.bytes));
            in.seekg(static_cast<std::streamoff>(ref.fileOffset), std::ios::beg);
            in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(ref.bytes));
            if (!in.good())
                throw AppException("RAD: cannot read chunk payload in " + path);

            if (readU32LE(buf.data()) != kRadChunkMagic)
                throw AppException("RAD: bad chunk magic in " + path);
            const uint32_t cmetaLen = readU32LE(buf.data() + 4);
            const uint64_t cmetaStart = 8;
            if (!fitsWithin(cmetaStart, cmetaLen, buf.size()))
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
            if (!cmeta.is_object())
                throw AppException("RAD: chunk meta is not an object in " + path);

            DecodedChunk out;
            const uint64_t count = unsignedField(cmeta, "count", path);
            if (count > kMaxSplatsPerChunk)
                throw AppException("RAD: chunk declares " + std::to_string(count) + " splats in " + path +
                                   " (at most " + std::to_string(kMaxSplatsPerChunk) + " are read)");
            out.count = static_cast<size_t>(count);
            if (out.count == 0)
                return out;

            const uint64_t payloadStart = 8u + roundUp8(cmetaLen) + 8u; // + u64 payloadBytes field

            if (!cmeta.contains("properties") || !cmeta["properties"].is_array())
                throw AppException("RAD: chunk has no properties array");

            try
            {
                for (const auto &p : cmeta["properties"])
                {
                    if (!p.is_object())
                        throw AppException("RAD: a chunk property is not an object in " + path);
                    const std::string name = p.value("property", std::string());
                    const bool isCenter = name == "center";
                    const bool isRgb = name == "rgb";
                    const bool isAlpha = name == "alpha";
                    const bool isScales = name == "scales";
                    const bool isOrientation = name == "orientation";
                    if (!isCenter && !(wanted.appearance && (isRgb || isAlpha)) &&
                        !(wanted.shape && (isScales || isOrientation)))
                        continue;

                    const std::string encoding = p.value("encoding", std::string());
                    const uint64_t offset = unsignedField(p, "offset", path);
                    const uint64_t bytes = unsignedField(p, "bytes", path);
                    PropertyRange range;
                    range.min = p.value("min", 0.0f);
                    range.max = p.value("max", 1.0f);
                    range.declared = p.contains("min") && p.contains("max");
                    const std::string compression = p.value("compression", std::string());

                    if (!fitsWithin(payloadStart, offset, buf.size()) ||
                        !fitsWithin(payloadStart + offset, bytes, buf.size()))
                        throw AppException("RAD: property '" + name + "' out of range in " + path);
                    const size_t dataStart = static_cast<size_t>(payloadStart + offset);

                    std::vector<uint8_t> raw;
                    if (compression == "gz")
                    {
                        // 4 bytes is the widest value of the encodings: nothing declared is larger
                        const int dims = isAlpha ? 1 : 3;
                        raw = inflateRaw(buf.data() + dataStart, static_cast<size_t>(bytes),
                                         static_cast<size_t>(dims) * out.count * 4);
                    }
                    else
                    {
                        raw.assign(buf.begin() + dataStart, buf.begin() + dataStart + static_cast<size_t>(bytes));
                    }

                    if (isCenter)
                        out.centers = decodeProperty(raw, encoding, 3, out.count, range);
                    else if (isRgb)
                        out.colors = decodeProperty(raw, encoding, 3, out.count, range);
                    else if (isAlpha)
                        out.opacities = decodeProperty(raw, encoding, 1, out.count, range);
                    else if (isScales)
                        out.scales = decodeProperty(raw, encoding, 3, out.count, range);
                    else
                        out.rotations = decodeOrientation(raw, encoding, out.count);
                }
            }
            catch (const json::exception &e)
            {
                throw AppException(std::string("RAD: invalid chunk property in ") + path + ": " + e.what());
            }

            if (out.centers.size() < out.count * 3)
                throw AppException("RAD: chunk missing center data in " + path);
            return out;
        }

    } // namespace

    bool isRadPath(const std::string &filename)
    {
        return io::Path(io::fromUtf8(filename)).checkExtension({"rad"});
    }

    RadCoarseSplats readRadCoarseSplats(const fs::path &radPath, int maxChunks)
    {
        std::ifstream in(radPath, std::ios::binary);
        if (!in.good())
            throw AppException("RAD: cannot open " + io::toUtf8(radPath));

        const RadHeader header = readHeader(in, io::toUtf8(radPath));

        RadCoarseSplats result;
        const int limit = maxChunks <= 0 ? static_cast<int>(header.chunks.size())
                                         : std::min<int>(maxChunks, static_cast<int>(header.chunks.size()));
        for (int i = 0; i < limit; ++i)
        {
            DecodedChunk chunk = decodeChunk(in, header, header.chunks[i], io::toUtf8(radPath), RadAttributes{true, true});
            if (chunk.count == 0)
                continue;

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

            // A chunk without a shape is a set of points: scale 0 and no rotation
            if (chunk.scales.size() == chunk.count * 3)
                result.scales.insert(result.scales.end(), chunk.scales.begin(), chunk.scales.end());
            else
                result.scales.resize(result.count * 3, 0.0f);

            if (chunk.rotations.size() == chunk.count * 4)
                result.rotations.insert(result.rotations.end(), chunk.rotations.begin(), chunk.rotations.end());
            else
                for (size_t k = 0; k < chunk.count; ++k)
                    result.rotations.insert(result.rotations.end(), {0.0f, 0.0f, 0.0f, 1.0f});
        }

        if (result.count == 0)
            throw AppException("RAD: no splats decoded from " + io::toUtf8(radPath));
        return result;
    }

    bool computeRadBounds(const fs::path &radPath, std::array<double, 3> &outMin,
                          std::array<double, 3> &outMax)
    {
        std::ifstream in(radPath, std::ios::binary);
        if (!in.good())
            throw AppException("RAD: cannot open " + io::toUtf8(radPath));

        const RadHeader header = readHeader(in, io::toUtf8(radPath));

        outMin = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max()};
        outMax = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                  std::numeric_limits<double>::lowest()};

        bool any = false;
        for (const auto &ref : header.chunks)
        {
            DecodedChunk chunk = decodeChunk(in, header, ref, io::toUtf8(radPath), RadAttributes{});
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
