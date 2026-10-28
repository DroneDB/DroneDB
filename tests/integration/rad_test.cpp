/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The RAD reader on files of our own making: the sizes a RAD declares (its metadata, its chunks, their
// properties, the splats of a chunk, what a compressed property inflates to) come from the file, so
// each of them is checked against what the file holds before a buffer is made or a read is done.
// The thumbnail rendered from such a file is tested here too: the splats that RAD makes up are the
// ones (NaN, infinite) no exporter would write.

#include <zlib.h>

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

// The tile buffers take 8 bytes a pixel: a size no one asks for in good faith is refused, before the
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
