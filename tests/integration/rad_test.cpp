/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The RAD reader on files of our own making: the sizes a RAD declares (its metadata, its chunks, their
// properties, the splats of a chunk, what a compressed property inflates to) come from the file, so
// each of them is checked against what the file holds before a buffer is made or a read is done.
// The thumbnail rendered from such a file is tested here too: the splats that RAD makes up are the
// ones (NaN, infinite) no exporter would write.

#include <gdal.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "exceptions.h"
#include "gtest/gtest.h"
#include "json.h"
#include "rad.h"
#include "test.h"
#include "testarea.h"
#include "thumbs.h"

namespace {

using namespace ddb;

std::string le32(uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), sizeof(v)); }
std::string le64(uint64_t v) { return std::string(reinterpret_cast<const char*>(&v), sizeof(v)); }

std::string padTo8(std::string s) {
    s.resize((s.size() + 7u) & ~size_t(7), '\0');
    return s;
}

// A chunk: magic, metadata, then the payload behind its length
std::string chunkOf(const json& meta, const std::string& payload) {
    const std::string m = meta.dump();
    return "RADC" + le32(static_cast<uint32_t>(m.size())) + padTo8(m) + le64(payload.size()) + payload;
}

// A file: magic, the declared length of the metadata, the metadata, and the chunks behind it
void writeRad(const fs::path& path, const json& header, const std::string& chunks, bool lieAboutMetaLength = false) {
    const std::string m = header.dump();
    const uint32_t declared = lieAboutMetaLength ? 0xFFFFFFF0u : static_cast<uint32_t>(m.size());
    std::ofstream out(path.string(), std::ios::binary);
    const std::string content = "RAD0" + le32(declared) + padTo8(m) + chunks;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

json headerOf(const std::vector<std::pair<uint64_t, uint64_t>>& offsetsAndBytes) {
    json chunks = json::array();
    for (const auto& [offset, bytes] : offsetsAndBytes)
        chunks.push_back({{"offset", offset}, {"bytes", bytes}});
    return {{"chunks", chunks}};
}

// Planar f32 centres of two splats: x = {1, 3}, y = {2, 4}, z = {5, 7}
std::string twoCentres() {
    const float values[6] = {1, 3, 2, 4, 5, 7};
    return std::string(reinterpret_cast<const char*>(values), sizeof(values));
}

std::string deflateRaw(const std::string& data) {
    z_stream strm{};
    EXPECT_EQ(deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
    std::string out;
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    strm.avail_in = static_cast<uInt>(data.size());
    char buffer[64 * 1024];
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        strm.avail_out = sizeof(buffer);
        ret = deflate(&strm, Z_FINISH);
        out.append(buffer, sizeof(buffer) - strm.avail_out);
    } while (ret != Z_STREAM_END);
    deflateEnd(&strm);
    return out;
}

json centreProperty(uint64_t bytes, const char* compression = nullptr) {
    json p = {{"property", "center"}, {"encoding", "f32"}, {"offset", 0}, {"bytes", bytes}};
    if (compression != nullptr)
        p["compression"] = compression;
    return p;
}

// A file with the single chunk of the given metadata and payload
fs::path writeSingleChunk(TestArea& ta, const std::string& name, const json& chunkMeta, const std::string& payload) {
    const std::string chunk = chunkOf(chunkMeta, payload);
    const fs::path path = ta.getPath(name);
    writeRad(path, headerOf({{0, chunk.size()}}), chunk);
    return path;
}

// Both readers refuse the file with an AppException that says why
void expectRefused(const fs::path& path, const std::string& reason) {
    std::array<double, 3> bmin{}, bmax{};
    try {
        computeRadBounds(path, bmin, bmax);
        ADD_FAILURE() << "computeRadBounds accepted " << path << ", expected: " << reason;
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find(reason), std::string::npos) << e.what();
    }
    try {
        readRadCoarseSplats(path, 1);
        ADD_FAILURE() << "readRadCoarseSplats accepted " << path << ", expected: " << reason;
    } catch (const AppException& e) {
        EXPECT_NE(std::string(e.what()).find(reason), std::string::npos) << e.what();
    }
}

TEST(rad, aWellFormedFileIsRead) {
    TestArea ta(TEST_NAME);

    // Stored and compressed: the same splats
    for (const char* compression : {static_cast<const char*>(nullptr), "gz"}) {
        const std::string payload = compression == nullptr ? twoCentres() : deflateRaw(twoCentres());
        const fs::path path = writeSingleChunk(
            ta, std::string("ok_") + (compression ? "gz" : "raw") + ".rad",
            {{"count", 2}, {"properties", json::array({centreProperty(payload.size(), compression)})}}, payload);

        std::array<double, 3> bmin{}, bmax{};
        ASSERT_TRUE(computeRadBounds(path, bmin, bmax)) << (compression ? compression : "raw");
        EXPECT_EQ(bmin, (std::array<double, 3>{1, 2, 5}));
        EXPECT_EQ(bmax, (std::array<double, 3>{3, 4, 7}));
        EXPECT_EQ(readRadCoarseSplats(path, 1).count, 2u);
    }
}

TEST(rad, aMetadataLengthBeyondTheFileIsRefusedBeforeAnythingIsAllocatedForIt) {
    TestArea ta(TEST_NAME);
    const fs::path path = ta.getPath("meta.rad");
    writeRad(path, headerOf({{0, 16}}), std::string(64, '\0'), /*lieAboutMetaLength=*/true);
    expectRefused(path, "declares 4294967280 bytes of metadata");
}

TEST(rad, aChunkBeyondTheEndOfTheFileIsRefused) {
    TestArea ta(TEST_NAME);
    const std::string chunk = chunkOf({{"count", 2}, {"properties", json::array()}}, "");

    const fs::path past = ta.getPath("offset_past_end.rad");
    writeRad(past, headerOf({{1000000, chunk.size()}}), chunk);
    expectRefused(past, "is beyond the end of");

    const fs::path longer = ta.getPath("bytes_past_end.rad");
    writeRad(longer, headerOf({{0, chunk.size() + 1}}), chunk);
    expectRefused(longer, "is beyond the end of");

    // The size is of a petabyte: no buffer is made for it
    const fs::path huge = ta.getPath("bytes_huge.rad");
    writeRad(huge, headerOf({{0, uint64_t(1) << 50}}), chunk);
    expectRefused(huge, "is beyond the end of");
}

TEST(rad, anOffsetAndASizeThatOverflowWhenAddedAreRefused) {
    TestArea ta(TEST_NAME);
    const std::string chunk = chunkOf({{"count", 2}, {"properties", json::array()}}, "");
    const uint64_t max = std::numeric_limits<uint64_t>::max();

    // offset + start wraps around to a small number
    const fs::path offset = ta.getPath("offset_wraps.rad");
    writeRad(offset, headerOf({{max, 16}}), chunk);
    expectRefused(offset, "is beyond the end of");

    // offset + bytes wraps around to a small number
    const fs::path bytes = ta.getPath("bytes_wraps.rad");
    writeRad(bytes, headerOf({{16, max - 8}}), chunk);
    expectRefused(bytes, "is beyond the end of");

    // a negative number is not wrapped into a huge one
    const fs::path negative = ta.getPath("negative.rad");
    writeRad(negative, {{"chunks", json::array({{{"offset", -8}, {"bytes", 16}}})}}, chunk);
    expectRefused(negative, "'offset' is not an unsigned integer");
}

TEST(rad, aPropertyBeyondItsChunkIsRefused) {
    TestArea ta(TEST_NAME);
    const uint64_t max = std::numeric_limits<uint64_t>::max();

    json past = centreProperty(24);
    past["offset"] = 4096;
    expectRefused(writeSingleChunk(ta, "property_past_end.rad", {{"count", 2}, {"properties", json::array({past})}},
                                   twoCentres()),
                  "property 'center' out of range");

    json wraps = centreProperty(24);
    wraps["offset"] = max - 4;
    expectRefused(writeSingleChunk(ta, "property_wraps.rad", {{"count", 2}, {"properties", json::array({wraps})}},
                                   twoCentres()),
                  "property 'center' out of range");

    json huge = centreProperty(max);
    expectRefused(writeSingleChunk(ta, "property_bytes_wraps.rad", {{"count", 2}, {"properties", json::array({huge})}},
                                   twoCentres()),
                  "property 'center' out of range");
}

TEST(rad, aChunkThatDeclaresMoreSplatsThanItCanHoldIsRefused) {
    TestArea ta(TEST_NAME);
    expectRefused(writeSingleChunk(ta, "many.rad",
                                   {{"count", uint64_t(1) << 40}, {"properties", json::array({centreProperty(24)})}},
                                   twoCentres()),
                  "chunk declares 1099511627776 splats");
}

// A few kilobytes that inflate to far more than the chunk declares (2 splats: 24 bytes of centres)
TEST(rad, aCompressedPropertyThatInflatesBeyondItsDeclaredSizeIsRefused) {
    TestArea ta(TEST_NAME);
    const std::string bomb = deflateRaw(std::string(256u * 1024u * 1024u, '\0'));
    ASSERT_LT(bomb.size(), 1024u * 1024u) << "the fixture is small";

    expectRefused(writeSingleChunk(ta, "bomb.rad",
                                   {{"count", 2}, {"properties", json::array({centreProperty(bomb.size(), "gz")})}},
                                   bomb),
                  "inflates to more than its declared 24 bytes");
}

TEST(rad, aMetadataValueOfTheWrongTypeIsAnAppException) {
    TestArea ta(TEST_NAME);
    expectRefused(writeSingleChunk(ta, "wrong_type.rad",
                                   {{"count", 2}, {"properties", json::array({{{"property", 5}}})}}, twoCentres()),
                  "invalid chunk property");
}

// ---------------------------------------------------------------------------
// The shape of the splats: scales and orientation
// ---------------------------------------------------------------------------

std::string floatsOf(const std::vector<float>& values) {
    return std::string(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float));
}

std::string halvesOf(const std::vector<uint16_t>& values) {
    return std::string(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(uint16_t));
}

// One property of a chunk: its metadata, and its bytes
struct Property {
    json meta;
    std::string payload;
};

Property propertyOf(const char* name, const char* encoding, const std::string& payload, json extra = json::object()) {
    json meta = {{"property", name}, {"encoding", encoding}, {"bytes", payload.size()}};
    meta.update(extra);
    return {meta, payload};
}

// A file whose single chunk has these properties, one after the other
fs::path writeProperties(TestArea& ta, const std::string& name, size_t count, const std::vector<Property>& properties) {
    json metas = json::array();
    std::string payload;
    for (const auto& property : properties) {
        json meta = property.meta;
        meta["offset"] = payload.size();
        metas.push_back(meta);
        payload += property.payload;
    }
    return writeSingleChunk(ta, name, {{"count", count}, {"properties", metas}}, payload);
}

const Property twoCentresProperty = propertyOf("center", "f32", twoCentres());

TEST(rad, theScalesOfASplatAreDecodedWhateverTheEncoding) {
    TestArea ta(TEST_NAME);

    // f32: the standard deviations as they are (planar: x of both splats, then y, then z)
    {
        const auto rad = writeProperties(
            ta, "scales_f32.rad", 2,
            {twoCentresProperty, propertyOf("scales", "f32", floatsOf({0.5f, 2.0f, 0.25f, 4.0f, 1.0f, 8.0f}))});
        const RadCoarseSplats splats = readRadCoarseSplats(rad, 1);
        ASSERT_EQ(splats.scales.size(), 6u);
        const std::vector<float> expected = {0.5f, 0.25f, 1.0f, 2.0f, 4.0f, 8.0f}; // per splat: x y z
        EXPECT_EQ(splats.scales, expected);
    }

    // ln_f16: the logarithm as a half float (0x3C00 is 1, 0x4000 is 2, 0xBC00 is -1, 0 is 0)
    {
        const auto rad = writeProperties(
            ta, "scales_ln_f16.rad", 2,
            {twoCentresProperty, propertyOf("scales", "ln_f16", halvesOf({0x0000, 0x3C00, 0xBC00, 0x4000, 0x0000, 0x0000}))});
        const RadCoarseSplats splats = readRadCoarseSplats(rad, 1);
        ASSERT_EQ(splats.scales.size(), 6u);
        const std::vector<float> expected = {1.0f, std::exp(-1.0f), 1.0f, std::exp(1.0f), std::exp(2.0f), 1.0f};
        for (size_t i = 0; i < expected.size(); ++i)
            EXPECT_NEAR(splats.scales[i], expected[i], 1e-3f) << i;
    }

    // ln_0r8: 0 is a scale of 0, 1..255 span the logarithms min..max
    {
        const std::string bytes = {char(0), char(1), char(128), char(255), char(255), char(1)};
        const auto rad = writeProperties(ta, "scales_ln_0r8.rad", 2,
                                         {twoCentresProperty, propertyOf("scales", "ln_0r8", bytes, {{"min", -2.0}, {"max", 2.0}})});
        const RadCoarseSplats splats = readRadCoarseSplats(rad, 1);
        ASSERT_EQ(splats.scales.size(), 6u);
        // splat 0: bytes 0, 128, 255 (x, y, z); splat 1: 1, 255, 1
        EXPECT_EQ(splats.scales[0], 0.0f);
        EXPECT_NEAR(splats.scales[1], 1.0f, 1e-4f);
        EXPECT_NEAR(splats.scales[2], std::exp(2.0f), 1e-3f);
        EXPECT_NEAR(splats.scales[3], std::exp(-2.0f), 1e-4f);
        EXPECT_NEAR(splats.scales[4], std::exp(2.0f), 1e-3f);
        EXPECT_NEAR(splats.scales[5], std::exp(-2.0f), 1e-4f);
    }

    // Its logarithms are not guessed when the property does not give them
    EXPECT_THROW(readRadCoarseSplats(writeProperties(ta, "scales_no_range.rad", 2,
                                                     {twoCentresProperty, propertyOf("scales", "ln_0r8", std::string(6, '\1'))}),
                                     1),
                 AppException);
}

TEST(rad, theOrientationOfASplatIsDecodedWhateverTheEncoding) {
    TestArea ta(TEST_NAME);

    // oct88r8: three bytes a splat, the axis (two octahedral coordinates) and the angle. Zero
    // degrees is the identity whatever the axis; 255 is 180 degrees about it. Axis +z is the centre
    // of the octahedron (127 is as close to it as a byte gets), +x its corner (255, 127).
    {
        const std::string bytes = {char(127), char(127), char(0),     // identity
                                   char(127), char(127), char(255)};  // 180 degrees about +z
        const auto rad = writeProperties(ta, "orientation_oct.rad", 2,
                                         {twoCentresProperty, propertyOf("orientation", "oct88r8", bytes)});
        const RadCoarseSplats splats = readRadCoarseSplats(rad, 1);
        ASSERT_EQ(splats.rotations.size(), 8u);
        EXPECT_NEAR(splats.rotations[3], 1.0f, 1e-5f);
        EXPECT_NEAR(std::abs(splats.rotations[0]) + std::abs(splats.rotations[1]) + std::abs(splats.rotations[2]), 0.0f, 1e-5f);
        EXPECT_NEAR(splats.rotations[4], 0.0f, 0.02f);
        EXPECT_NEAR(splats.rotations[5], 0.0f, 0.02f);
        EXPECT_NEAR(splats.rotations[6], 1.0f, 0.02f);
        EXPECT_NEAR(splats.rotations[7], 0.0f, 0.02f);
    }

    // 90 degrees about +x (255, 127, angle byte 128 is 90.2 degrees)
    {
        const std::string bytes = {char(255), char(127), char(128), char(255), char(127), char(0)};
        const auto rad = writeProperties(ta, "orientation_oct_x.rad", 2,
                                         {twoCentresProperty, propertyOf("orientation", "oct88r8", bytes)});
        const RadCoarseSplats splats = readRadCoarseSplats(rad, 1);
        ASSERT_EQ(splats.rotations.size(), 8u);
        const float half = 128.0f / 255.0f * 0.5f * 3.14159265f;
        EXPECT_NEAR(splats.rotations[0], std::sin(half), 0.01f);
        EXPECT_NEAR(splats.rotations[1], 0.0f, 0.01f);
        EXPECT_NEAR(splats.rotations[2], 0.0f, 0.01f);
        EXPECT_NEAR(splats.rotations[3], std::cos(half), 0.01f);
        EXPECT_NEAR(splats.rotations[0] * splats.rotations[0] + splats.rotations[1] * splats.rotations[1] +
                        splats.rotations[2] * splats.rotations[2] + splats.rotations[3] * splats.rotations[3],
                    1.0f, 1e-3f) << "a unit quaternion";
    }

    // f32: x, y, z, and w to make a unit quaternion of them
    {
        const auto rad = writeProperties(
            ta, "orientation_f32.rad", 2,
            {twoCentresProperty, propertyOf("orientation", "f32", floatsOf({0.5f, 0.0f, 0.5f, 0.0f, 0.5f, 1.0f}))});
        const RadCoarseSplats splats = readRadCoarseSplats(rad, 1);
        const std::vector<float> expected = {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f};
        ASSERT_EQ(splats.rotations.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            EXPECT_NEAR(splats.rotations[i], expected[i], 1e-6f) << i;
    }

    // A property the reader does not know is refused, not read as something else
    EXPECT_THROW(readRadCoarseSplats(writeProperties(ta, "orientation_unknown.rad", 2,
                                                     {twoCentresProperty, propertyOf("orientation", "s8", std::string(6, '\0'))}),
                                     1),
                 AppException);
}

// A chunk made of points only: no extent, no rotation
TEST(rad, aSplatWithoutAShapeHasNoExtentAndNoRotation) {
    TestArea ta(TEST_NAME);
    const RadCoarseSplats splats = readRadCoarseSplats(writeProperties(ta, "points.rad", 2, {twoCentresProperty}), 1);
    EXPECT_EQ(splats.scales, std::vector<float>(6, 0.0f));
    EXPECT_EQ(splats.rotations, (std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1}));
}

// The shape is compressed like the other properties, and held to its declared size
TEST(rad, aCompressedShapeIsInflatedWithinItsDeclaredSize) {
    TestArea ta(TEST_NAME);
    const std::string scales = floatsOf({0.5f, 2.0f, 0.25f, 4.0f, 1.0f, 8.0f});
    const auto rad = writeProperties(
        ta, "scales_gz.rad", 2,
        {twoCentresProperty, propertyOf("scales", "f32", deflateRaw(scales), {{"compression", "gz"}})});
    EXPECT_EQ(readRadCoarseSplats(rad, 1).scales.size(), 6u);

    const std::string bomb = deflateRaw(std::string(64u * 1024u * 1024u, '\0'));
    EXPECT_THROW(readRadCoarseSplats(writeProperties(ta, "scales_bomb.rad", 2,
                                                     {twoCentresProperty, propertyOf("scales", "f32", bomb, {{"compression", "gz"}})}),
                                     1),
                 AppException);
}

// ---------------------------------------------------------------------------
// The thumbnail of a RAD
// ---------------------------------------------------------------------------

using Centre = std::array<float, 3>;

// A RAD of the given splat centres (planar f32, as twoCentres()); colour and opacity default
fs::path writeCentres(TestArea& ta, const std::string& name, const std::vector<Centre>& centres) {
    std::string payload;
    for (int axis = 0; axis < 3; ++axis)
        for (const auto& c : centres)
            payload.append(reinterpret_cast<const char*>(&c[axis]), sizeof(float));
    return writeSingleChunk(
        ta, name, {{"count", centres.size()}, {"properties", json::array({centreProperty(payload.size())})}},
        payload);
}

std::string thumbnailOf(const fs::path& rad, const fs::path& image, int size = 64) {
    generateSplatThumbFromRad(rad, size, image, nullptr, nullptr);
    std::ifstream in(image.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A splat at NaN or infinity has no pixel: it is left out of the frame and not drawn, so the file
// with such splats renders as the file without them (the conversion of its position to a pixel was
// undefined, and an infinite one made the whole frame infinite)
TEST(rad, aSplatWithoutAFinitePositionIsLeftOutOfTheThumbnail) {
    TestArea ta(TEST_NAME);
    const std::vector<Centre> good = {{0, 0, 0}, {10, 0, 0.5f}, {0, 10, 0}, {10, 10, 0.5f}, {5, 5, 0.2f}};
    std::vector<Centre> withBad = good;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    withBad.push_back({nan, 3, 0});
    withBad.push_back({4, inf, 0});
    withBad.push_back({4, 4, -inf});

    const std::string clean = thumbnailOf(writeCentres(ta, "good.rad", good), ta.getPath("good.webp"));
    const std::string dirty = thumbnailOf(writeCentres(ta, "bad.rad", withBad), ta.getPath("bad.webp"));
    ASSERT_FALSE(clean.empty());
    EXPECT_EQ(clean, dirty);
}

// Failures of the data are AppExceptions, not the GDAL errors of a library that is not involved
TEST(rad, aThumbnailThatCannotBeRenderedFailsWithAnAppExceptionOfItsOwn) {
    TestArea ta(TEST_NAME);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto failure = [&](const std::string& name, const std::vector<Centre>& centres) {
        try {
            generateSplatThumbFromRad(writeCentres(ta, name, centres), 64, ta.getPath(name + ".webp"), nullptr,
                                      nullptr);
        } catch (const GDALException&) {
            ADD_FAILURE() << name << ": a GDALException for a failure that is not GDAL's";
        } catch (const AppException& e) {
            return std::string(e.what());
        }
        return std::string();
    };

    // All splats at one point: no extent to draw; none of them finite: no frame at all
    EXPECT_NE(failure("point.rad", {{1, 2, 3}, {1, 2, 3}}).find("zero planar extent"), std::string::npos);
    EXPECT_NE(failure("nan.rad", {{nan, 0, 0}, {0, nan, 0}}).find("no finite position"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The picture of the splats: Gaussian footprints, composited back to front
// ---------------------------------------------------------------------------

struct Picture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    const uint8_t* at(int x, int y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

Picture decodePicture(const fs::path& image) {
    Picture picture;
    GDALDatasetH ds = GDALOpen(image.string().c_str(), GA_ReadOnly);
    EXPECT_NE(ds, nullptr) << image;
    if (ds == nullptr)
        return picture;
    picture.width = GDALGetRasterXSize(ds);
    picture.height = GDALGetRasterYSize(ds);
    const int bands = GDALGetRasterCount(ds);
    picture.rgba.assign(static_cast<size_t>(picture.width) * picture.height * 4, 255);
    std::vector<uint8_t> plane(static_cast<size_t>(picture.width) * picture.height);
    for (int b = 0; b < std::min(bands, 4); ++b) {
        EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(ds, b + 1), GF_Read, 0, 0, picture.width, picture.height, plane.data(),
                               picture.width, picture.height, GDT_Byte, 0, 0),
                  CE_None);
        for (size_t i = 0; i < plane.size(); ++i)
            picture.rgba[i * 4 + b] = plane[i];
    }
    GDALClose(ds);
    return picture;
}

struct SplatSpec {
    Centre centre;
    std::array<float, 3> colour;
    float opacity;
    std::array<float, 3> scale = {0, 0, 0};
    std::array<float, 3> rotationXyz = {0, 0, 0}; // w completes the unit quaternion
};

// A RAD of these splats: centre, colour, opacity, scale and rotation as f32
fs::path writeSplats(TestArea& ta, const std::string& name, const std::vector<SplatSpec>& splats) {
    const auto planar = [&](auto member, int dims) {
        std::vector<float> values;
        for (int d = 0; d < dims; ++d)
            for (const auto& splat : splats)
                values.push_back(member(splat, d));
        return floatsOf(values);
    };
    return writeProperties(
        ta, name, splats.size(),
        {propertyOf("center", "f32", planar([](const SplatSpec& s, int d) { return s.centre[d]; }, 3)),
         propertyOf("rgb", "f32", planar([](const SplatSpec& s, int d) { return s.colour[d]; }, 3)),
         propertyOf("alpha", "f32", planar([](const SplatSpec& s, int) { return s.opacity; }, 1)),
         propertyOf("scales", "f32", planar([](const SplatSpec& s, int d) { return s.scale[d]; }, 3)),
         propertyOf("orientation", "f32", planar([](const SplatSpec& s, int d) { return s.rotationXyz[d]; }, 3))});
}

// The corners of a square of 20 around the origin, as dots: they make the frame
std::vector<SplatSpec> frameCorners() {
    std::vector<SplatSpec> corners;
    for (const float x : {-10.0f, 10.0f})
        for (const float y : {-10.0f, 10.0f})
            corners.push_back({{x, y, 0}, {1, 1, 1}, 1.0f});
    return corners;
}

// The box of the pixels that are clearly red
struct Box {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
};

Box redBox(const Picture& picture) {
    Box box;
    for (int y = 0; y < picture.height; ++y)
        for (int x = 0; x < picture.width; ++x) {
            const uint8_t* p = picture.at(x, y);
            if (p[3] > 128 && p[0] > 150 && p[1] < 110 && p[2] < 110) {
                box.x0 = std::min(box.x0, x);
                box.x1 = std::max(box.x1, x);
                box.y0 = std::min(box.y0, y);
                box.y1 = std::max(box.y1, y);
            }
        }
    return box;
}

Picture pictureOf(TestArea& ta, const fs::path& rad, int size = 160) {
    const fs::path image = ta.getPath(rad.stem().string() + ".webp");
    generateSplatThumbFromRad(rad, size, image, nullptr, nullptr);
    return decodePicture(image);
}

// A splat is drawn as the ellipse it is: long along the axis of its larger scale, and turned with it
TEST(rad, aSplatIsDrawnAsAnEllipseAlongItsScaledAxis) {
    TestArea ta(TEST_NAME);
    auto splats = frameCorners();

    // Wide along x
    splats.push_back({{0, 0, 0}, {1, 0, 0}, 1.0f, {3.0f, 0.5f, 0.5f}});
    const Box wide = redBox(pictureOf(ta, writeSplats(ta, "wide.rad", splats)));
    ASSERT_GT(wide.x1, 0) << "the splat is in the picture";
    EXPECT_GT(wide.width(), 3 * wide.height()) << wide.width() << " x " << wide.height();

    // The same splat turned 90 degrees about the line of sight: tall
    splats.back().rotationXyz = {0, 0, 0.70710678f};
    const Box tall = redBox(pictureOf(ta, writeSplats(ta, "tall.rad", splats)));
    ASSERT_GT(tall.x1, 0);
    EXPECT_GT(tall.height(), 3 * tall.width()) << tall.width() << " x " << tall.height();

    // A round splat is round, and a splat with no extent is a dot, not a disc of a size of its own
    splats.back() = {{0, 0, 0}, {1, 0, 0}, 1.0f, {1.5f, 1.5f, 1.5f}};
    const Box round = redBox(pictureOf(ta, writeSplats(ta, "round.rad", splats)));
    ASSERT_GT(round.x1, 0);
    EXPECT_NEAR(static_cast<double>(round.width()) / round.height(), 1.0, 0.2);
    EXPECT_GT(round.width(), 2 * wide.height());
    splats.back() = {{0, 0, 0}, {1, 0, 0}, 1.0f, {0, 0, 0}};
    const Box dot = redBox(pictureOf(ta, writeSplats(ta, "dot.rad", splats)));
    EXPECT_LT(dot.width(), round.width() / 3);
}

// Splats are composited from the far one to the near one: the opaque splat in front hides the one
// behind it, whatever order the file lists them in
TEST(rad, aSplatInFrontHidesTheOneBehind) {
    TestArea ta(TEST_NAME);
    // The scene is flat along z, the shortest axis, which the picture looks along from its top
    const SplatSpec red = {{0, 0, 0}, {1, 0, 0}, 1.0f, {2, 2, 2}};
    const SplatSpec green = {{0, 0, 0}, {0, 1, 0}, 1.0f, {2, 2, 2}};
    const auto centre = [](const Picture& picture) { return picture.at(picture.width / 2, picture.height / 2); };

    for (const bool redInFront : {true, false}) {
        for (const bool redFirst : {true, false}) {
            auto splats = frameCorners();
            SplatSpec a = red, b = green;
            (redInFront ? a : b).centre[2] = 1.0f;
            if (redFirst) { splats.push_back(a); splats.push_back(b); }
            else { splats.push_back(b); splats.push_back(a); }

            const Picture picture = pictureOf(ta, writeSplats(ta, "order.rad", splats));
            const uint8_t* p = centre(picture);
            SCOPED_TRACE(std::string(redInFront ? "red" : "green") + " in front, " + (redFirst ? "red" : "green") + " first");
            if (redInFront) {
                EXPECT_GT(p[0], 200);
                EXPECT_LT(p[1], 80);
            } else {
                EXPECT_GT(p[1], 200);
                EXPECT_LT(p[0], 80);
            }
        }
    }
}

// The picture is as large as it is asked to be
TEST(rad, theThumbnailHasTheSizeAskedFor) {
    TestArea ta(TEST_NAME);
    auto splats = frameCorners();
    splats.push_back({{0, 0, 0}, {1, 0, 0}, 1.0f, {2, 2, 2}});
    const fs::path rad = writeSplats(ta, "sized.rad", splats);
    for (const int size : {1, 64, 200, 513}) {
        const fs::path image = ta.getPath("sized_" + std::to_string(size) + ".webp");
        generateSplatThumbFromRad(rad, size, image, nullptr, nullptr);
        const Picture picture = decodePicture(image);
        EXPECT_EQ(picture.width, size);
        EXPECT_EQ(picture.height, size);
    }
}

// A splat with no opacity is not drawn
TEST(rad, aTransparentSplatLeavesNoTrace) {
    TestArea ta(TEST_NAME);
    auto splats = frameCorners();
    splats.push_back({{0, 0, 0}, {1, 0, 0}, 0.0f, {2, 2, 2}});
    EXPECT_EQ(redBox(pictureOf(ta, writeSplats(ta, "invisible.rad", splats))).x1, -1);
}

// The tile buffers take 20 bytes a pixel: a size no one asks for in good faith is refused, before the
// file is read
TEST(rad, aThumbnailLargerThanTheCapIsRefused) {
    TestArea ta(TEST_NAME);
    const fs::path rad = writeCentres(ta, "scene.rad", {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}});
    for (const int size : {0, -1, 4097, 100000, std::numeric_limits<int>::max()}) {
        SCOPED_TRACE(size);
        EXPECT_THROW(generateSplatThumbFromRad(rad, size, ta.getPath("big.webp"), nullptr, nullptr),
                     InvalidArgsException);
        EXPECT_FALSE(fs::exists(ta.getPath("big.webp")));
    }
    // Refused before the file is opened, so a file that is not there is not what it reports
    EXPECT_THROW(generateSplatThumbFromRad(ta.getPath("missing.rad"), 5000, ta.getPath("big.webp"), nullptr, nullptr),
                 InvalidArgsException);

    // A size within the cap renders
    EXPECT_NO_THROW(generateSplatThumbFromRad(rad, 512, ta.getPath("ok.webp"), nullptr, nullptr));
}

} // namespace
