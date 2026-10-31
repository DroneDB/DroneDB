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
#include <random>
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

// The splats of a level-of-detail tree: 0 is refined by 1 and 2 (read), 3 by splats that are not
// in the chunks read, 1, 2 and 4 are leaves
TEST(rad, theCutLeavesOutTheSplatsThatOthersReadRefine) {
    TestArea ta(TEST_NAME);
    const auto u16 = [](const std::vector<uint16_t>& v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size() * 2); };
    const auto u32 = [](const std::vector<uint32_t>& v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size() * 4); };
    const auto rad = writeProperties(
        ta, "tree.rad", 5,
        {propertyOf("center", "f32", floatsOf({0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})),
         propertyOf("child_count", "u16", u16({2, 0, 0, 3, 0})),
         propertyOf("child_start", "u32", u32({1, 0, 0, 100, 0}))});

    const RadCoarseSplats all = readRadCoarseSplats(rad, 1);
    EXPECT_EQ(all.count, 5u) << "without the cut every splat is read";

    const RadCoarseSplats cut = readRadCoarseSplats(rad, 1, /*onlyTheCut=*/true);
    ASSERT_EQ(cut.count, 4u);
    EXPECT_EQ(cut.positions.size(), 12u);
    EXPECT_EQ(cut.scales.size(), 12u);
    EXPECT_EQ(cut.rotations.size(), 16u);
    EXPECT_EQ(cut.opacities.size(), 4u);
    const std::vector<float> kept = {cut.positions[0], cut.positions[3], cut.positions[6], cut.positions[9]};
    EXPECT_EQ(kept, (std::vector<float>{1, 2, 3, 4}));

    // A file without a tree has nothing refined
    EXPECT_EQ(readRadCoarseSplats(writeProperties(ta, "flat.rad", 2, {twoCentresProperty}), 1, true).count, 2u);
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

// The corners of a square of 40 around the origin, as dots: they make the frame, wide enough for the
// splats of the tests to be a small part of the picture (a splat wider than an eighth of it is a blur,
// and is not drawn). A scene of so few splats has no up vector of its own: it stands on the viewer's
// default, -Y, and is seen from above it.
std::vector<SplatSpec> frameCorners() {
    std::vector<SplatSpec> corners;
    for (const float x : {-20.0f, 20.0f})
        for (const float y : {-20.0f, 20.0f})
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

// The red pixels as a shape: how many, how much longer than wide (the ratio of their spreads along
// their principal axes) and the angle of the longer axis in the picture, in degrees
struct Shape {
    size_t pixels = 0;
    double elongation = 0.0;
    double angle = 0.0;
};

Shape redShape(const Picture& picture) {
    Shape shape;
    double sx = 0, sy = 0;
    std::vector<std::pair<int, int>> red;
    for (int y = 0; y < picture.height; ++y)
        for (int x = 0; x < picture.width; ++x) {
            const uint8_t* p = picture.at(x, y);
            if (p[3] > 128 && p[0] > 150 && p[1] < 110 && p[2] < 110) {
                red.emplace_back(x, y);
                sx += x;
                sy += y;
            }
        }
    shape.pixels = red.size();
    if (red.empty())
        return shape;
    const double count = static_cast<double>(red.size());
    const double mx = sx / count, my = sy / count;
    double cxx = 0, cxy = 0, cyy = 0;
    for (const auto& [x, y] : red) {
        cxx += (x - mx) * (x - mx);
        cxy += (x - mx) * (y - my);
        cyy += (y - my) * (y - my);
    }
    const double mean = (cxx + cyy) / 2, spread = std::sqrt((cxx - cyy) * (cxx - cyy) / 4 + cxy * cxy);
    shape.elongation = std::sqrt((mean + spread) / std::max(mean - spread, 1e-9));
    shape.angle = 0.5 * std::atan2(2 * cxy, cxx - cyy) * 180.0 / 3.14159265358979323846;
    return shape;
}

// The angle between two axes of the picture, in degrees from 0 to 90
double degreesBetween(double a, double b) {
    const double d = std::fmod(std::abs(a - b), 180.0);
    return std::min(d, 180.0 - d);
}

// A splat is drawn as the ellipse it is: long along the axis of its larger scale, and turned with it.
// (The picture is a three-quarter view: an axis of the scene is seen at a slant, so the shapes are
// measured along their own axes, not along those of the picture.)
TEST(rad, aSplatIsDrawnAsAnEllipseAlongItsScaledAxis) {
    TestArea ta(TEST_NAME);
    auto splats = frameCorners();

    // Long along x
    splats.push_back({{0, 0, 0}, {1, 0, 0}, 1.0f, {3.0f, 0.5f, 0.5f}});
    const Shape alongX = redShape(pictureOf(ta, writeSplats(ta, "along_x.rad", splats)));
    ASSERT_GT(alongX.pixels, 0u) << "the splat is in the picture";
    EXPECT_GT(alongX.elongation, 3.0);

    // The same splat turned 90 degrees about z: long along y, the up the scene stands on
    splats.back().rotationXyz = {0, 0, 0.70710678f};
    const Shape alongY = redShape(pictureOf(ta, writeSplats(ta, "along_y.rad", splats)));
    ASSERT_GT(alongY.pixels, 0u);
    EXPECT_GT(alongY.elongation, 3.0);
    EXPECT_GT(degreesBetween(alongX.angle, alongY.angle), 30.0) << alongX.angle << " and " << alongY.angle;
    EXPECT_LT(degreesBetween(alongY.angle, 90.0), 10.0) << "up is up the picture: " << alongY.angle;

    // A round splat is round, and a splat with no extent is a dot, not a disc of a size of its own
    splats.back() = {{0, 0, 0}, {1, 0, 0}, 1.0f, {1.5f, 1.5f, 1.5f}};
    const Shape round = redShape(pictureOf(ta, writeSplats(ta, "round.rad", splats)));
    ASSERT_GT(round.pixels, 0u);
    EXPECT_LT(round.elongation, 1.3);
    EXPECT_GT(round.pixels, alongX.pixels);
    splats.back() = {{0, 0, 0}, {1, 0, 0}, 1.0f, {0, 0, 0}};
    EXPECT_LT(redShape(pictureOf(ta, writeSplats(ta, "dot.rad", splats))).pixels, round.pixels / 9);
}

// The opaque splat in front hides the one behind it, whatever order the file lists them in
TEST(rad, aSplatInFrontHidesTheOneBehind) {
    TestArea ta(TEST_NAME);
    // The camera looks down on the scene from above its up, -Y: the higher splat (lower y) is in front
    const SplatSpec red = {{0, 0, 0}, {1, 0, 0}, 1.0f, {2, 2, 2}};
    const SplatSpec green = {{0, 0, 0}, {0, 1, 0}, 1.0f, {2, 2, 2}};
    const auto centre = [](const Picture& picture) { return picture.at(picture.width / 2, picture.height / 2); };

    for (const bool redInFront : {true, false}) {
        for (const bool redFirst : {true, false}) {
            auto splats = frameCorners();
            SplatSpec a = red, b = green;
            (redInFront ? a : b).centre[1] = -1.0f;
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

// ---------------------------------------------------------------------------
// The view: the subject, not the floaters; rooms opened; blurs left out
// ---------------------------------------------------------------------------

// A deterministic cloud of `count` splats spread evenly in the box [lo, hi]
std::vector<SplatSpec> splatsIn(std::mt19937& rng, size_t count, Centre lo, Centre hi, std::array<float, 3> colour,
                                float scale) {
    std::vector<SplatSpec> splats;
    for (size_t i = 0; i < count; ++i) {
        SplatSpec splat{{0, 0, 0}, colour, 0.9f, {scale, scale, scale}};
        for (int k = 0; k < 3; ++k)
            splat.centre[k] = std::uniform_real_distribution<float>(lo[k], hi[k])(rng);
        splats.push_back(splat);
    }
    return splats;
}

SplatThumbStats statsOf(TestArea& ta, const fs::path& rad, int size = 160) {
    SplatThumbStats stats;
    generateSplatThumbFromRad(rad, size, ta.getPath(rad.stem().string() + "_stats.webp"), nullptr, nullptr, &stats);
    return stats;
}

// The picture is aimed at the subject, the dense part of the scene: floaters scattered far around it
// (5% of the splats, more than the outermost splats any frame leaves out) leave the subject where it
// was in the picture and about as large. A frame fitted to all but the outermost 2% of the splats, as
// the picture had, shrank the subject to a few pixels.
TEST(rad, floatersDoNotChangeTheFrame) {
    TestArea ta(TEST_NAME);
    std::mt19937 rng(7);
    const auto subject = splatsIn(rng, 2000, {-4, -1, -4}, {4, 1, 4}, {1, 0, 0}, 0.15f);
    auto withFloaters = subject;
    for (const auto& floater : splatsIn(rng, 105, {-100, -100, -100}, {100, 100, 100}, {1, 1, 1}, 0.15f))
        withFloaters.push_back(floater);

    const Box clean = redBox(pictureOf(ta, writeSplats(ta, "clean.rad", subject)));
    const Box floating = redBox(pictureOf(ta, writeSplats(ta, "floaters.rad", withFloaters)));
    ASSERT_GT(clean.x1, 0);
    ASSERT_GT(floating.x1, 0) << "the subject is in the picture";
    EXPECT_GT(clean.width(), 80) << "the subject fills the picture";
    EXPECT_NEAR(floating.width(), clean.width(), 0.15 * clean.width());
    EXPECT_NEAR(floating.height(), clean.height(), 0.15 * clean.height());
    EXPECT_NEAR(floating.x0 + floating.x1, clean.x0 + clean.x1, 0.1 * 2 * 160);
    EXPECT_NEAR(floating.y0 + floating.y1, clean.y0 + clean.y1, 0.1 * 2 * 160);
}

// Splats are composited front to back, and a pixel is done once almost nothing behind it shows: the
// layers behind it are not painted, and the colour is the one compositing all of them gives (each
// layer's colour times its alpha times what the layers in front of it let through), within the
// rounding of the picture. Eleven layers of 0.75 opacity, red, green and blue in turn, one above the
// other along the up the camera looks down: what shows after seven of them is less than 1e-4.
TEST(rad, frontToBackCompositingEqualsCompositingEveryLayer) {
    TestArea ta(TEST_NAME);
    const std::array<std::array<float, 3>, 3> colours = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    constexpr int Layers = 11;
    constexpr float Opacity = 0.75f;
    auto splats = frameCorners();
    for (int k = 0; k < Layers; ++k)
        splats.push_back({{0, -0.02f * static_cast<float>(k), 0}, colours[k % 3], Opacity, {2, 2, 2}});

    // The highest layer is the nearest
    std::array<double, 3> expected{};
    double through = 1.0;
    for (int k = Layers - 1; k >= 0; --k) {
        for (int c = 0; c < 3; ++c)
            expected[c] += through * Opacity * colours[k % 3][c];
        through *= 1.0 - Opacity;
    }
    ASSERT_LT(std::pow(1.0 - Opacity, 7), 1e-4) << "the layers behind the 7th are not painted";

    const Picture picture = pictureOf(ta, writeSplats(ta, "layers.rad", splats), 400);
    const uint8_t* p = picture.at(picture.width / 2, picture.height / 2);
    EXPECT_GT(p[3], 250);
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(p[c], expected[c] / (1.0 - through) * 255.0, 6.0) << "channel " << c;
}

// A splat whose footprint is wider than an eighth of the picture is a blur, not a part of the scene:
// it is not drawn, and the splat it would cover shows
TEST(rad, aSplatTooWideForThePictureIsLeftOut) {
    TestArea ta(TEST_NAME);
    auto splats = frameCorners();
    splats.push_back({{0, 0, 0}, {1, 0, 0}, 1.0f, {2, 2, 2}});
    const SplatSpec blur = {{0, -0.5f, 0}, {0, 1, 0}, 1.0f, {30, 30, 30}}; // in front of the red one
    splats.push_back(blur);
    const fs::path rad = writeSplats(ta, "blur.rad", splats);

    const SplatThumbStats stats = statsOf(ta, rad);
    EXPECT_EQ(stats.culledOversized, 1u);
    const Picture picture = pictureOf(ta, rad);
    const uint8_t* p = picture.at(picture.width / 2, picture.height / 2);
    EXPECT_GT(p[0], 200);
    EXPECT_LT(p[1], 80);

    // A smaller one is drawn, and hides the red one
    splats.back().scale = {2, 2, 2};
    const Picture covered = pictureOf(ta, writeSplats(ta, "small.rad", splats));
    EXPECT_GT(covered.at(covered.width / 2, covered.height / 2)[1], 200);
}

// A room is seen from outside and above, with its ceiling cut away, so what stands in it shows; a
// subject in the open air is not cut so
TEST(rad, aRoomIsOpenedFromAbove) {
    TestArea ta(TEST_NAME);
    std::mt19937 rng(11);
    const std::array<float, 3> grey = {0.5f, 0.5f, 0.5f};
    // The floor at y = 0, the ceiling at y = -3 (up is -Y), the walls around 10 x 10
    std::vector<SplatSpec> room;
    for (const auto& part : {splatsIn(rng, 600, {-5, 0, -5}, {5, 0, 5}, grey, 0.2f),
                             splatsIn(rng, 600, {-5, -3, -5}, {5, -3, 5}, grey, 0.2f),
                             splatsIn(rng, 200, {-5, -3, -5}, {-5, 0, 5}, grey, 0.2f),
                             splatsIn(rng, 200, {5, -3, -5}, {5, 0, 5}, grey, 0.2f),
                             splatsIn(rng, 200, {-5, -3, -5}, {5, 0, -5}, grey, 0.2f),
                             splatsIn(rng, 200, {-5, -3, 5}, {5, 0, 5}, grey, 0.2f),
                             splatsIn(rng, 100, {-0.5f, -2, -0.5f}, {0.5f, -1, 0.5f}, {1, 0, 0}, 0.1f)})
        room.insert(room.end(), part.begin(), part.end());
    const fs::path rad = writeSplats(ta, "room.rad", room);

    const SplatThumbStats stats = statsOf(ta, rad);
    EXPECT_TRUE(stats.enclosed);
    EXPECT_GT(stats.cutAway, 300u) << "the ceiling is cut away";
    EXPECT_GT(redBox(pictureOf(ta, rad)).x1, 0) << "what stands in the room shows";

    // Without its ceiling and walls it is a subject on the ground, in the open air
    std::vector<SplatSpec> open(room.begin(), room.begin() + 600);
    open.insert(open.end(), room.end() - 100, room.end());
    EXPECT_FALSE(statsOf(ta, writeSplats(ta, "open.rad", open)).enclosed);
}

// The buffers of the compositing do not grow with the picture beyond a few bytes a pixel: the splats
// are composited a tile at a time, so only the picture itself is as large as the picture
TEST(rad, theCompositingNeedsNoBufferAsLargeAsThePicture) {
    TestArea ta(TEST_NAME);
    auto splats = frameCorners();
    splats.push_back({{0, 0, 0}, {1, 0, 0}, 1.0f, {2, 2, 2}});
    const fs::path rad = writeSplats(ta, "buffers.rad", splats);
    for (const int size : {64, 1024}) {
        SCOPED_TRACE(size);
        const SplatThumbStats stats = statsOf(ta, rad, size);
        const size_t pixels = static_cast<size_t>(size) * static_cast<size_t>(size);
        EXPECT_GT(stats.pixelBufferBytes, 0u);
        EXPECT_LE(stats.pixelBufferBytes, 8 * pixels);
    }
    // At a size where a buffer of a float a pixel would be 4 MiB, the tiles take a fraction of a byte a pixel
    EXPECT_LT(statsOf(ta, rad, 1024).pixelBufferBytes, 1024u * 1024u / 4u);
}

// A size no one asks for in good faith is refused, before the file is read
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
