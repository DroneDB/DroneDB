/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The splat format table is the one place that knows which extension is which splat format and what
// each is accepted for. These tests pin the table, the content sniffing written once beside it, and
// that the entry handler, the thumbnail generator and the converter's list read it instead of
// spelling their own.

#include <cstdint>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "build_layout.h"
#include "entry_handler.h"
#include "gtest/gtest.h"
#include "ply.h"
#include "splat_format.h"
#include "test.h"
#include "testarea.h"

namespace {

using namespace ddb;
using Extensions = std::vector<std::string>;

void writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path.string(), std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

TEST(splatFormat, EveryExtensionMapsToItsFormatWhateverItsCase) {
    const std::vector<std::pair<std::string, SplatFormat>> expected = {
        {"ply", SplatFormat::Ply},       {"splat", SplatFormat::Splat}, {"spz", SplatFormat::Spz},
        {"ksplat", SplatFormat::Ksplat}, {"sog", SplatFormat::Sog},     {"rad", SplatFormat::Rad}};
    for (const auto& [extension, format] : expected) {
        SCOPED_TRACE(extension);
        EXPECT_EQ(splatFormatOfExtension(extension), format);
        EXPECT_EQ(splatFormatOfExtension("." + extension), format);
        std::string upper = extension;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        EXPECT_EQ(splatFormatOfExtension(upper), format);
        EXPECT_EQ(splatFormatOf("some/dir/scene." + upper), format);
    }

    for (const char* other : {"", ".", "zip", "spzx", "laz", "obj"})
        EXPECT_EQ(splatFormatOfExtension(other), SplatFormat::Unknown) << other;
    EXPECT_EQ(splatFormatOf("noextension"), SplatFormat::Unknown);
    EXPECT_EQ(splatFormatOf("dir.spz/file"), SplatFormat::Unknown);
}

// What each format is accepted for, spelled out: changing a flag is a decision, not a refactoring
TEST(splatFormat, TheStagesEachFormatIsAcceptedForArePinned) {
    struct Row {
        SplatFormat format;
        bool detect, metadata, build, thumbnail, convert;
    };
    const std::vector<Row> rows = {
        // A PLY is told from a mesh or a point cloud by its header, so the extension does not detect it
        {SplatFormat::Ply, false, true, true, false, true},
        {SplatFormat::Splat, true, true, true, false, true},
        {SplatFormat::Spz, true, true, true, true, true},
        {SplatFormat::Ksplat, true, true, true, false, false},
        // Build input with no metadata reader: the splat count is inside the ZIP
        {SplatFormat::Sog, true, false, true, false, false},
        // What a build writes: a thumbnail source, not yet an upload that is a splat
        {SplatFormat::Rad, false, false, false, true, false},
        {SplatFormat::Unknown, false, false, false, false, false}};
    for (const auto& r : rows) {
        SCOPED_TRACE(static_cast<int>(r.format));
        EXPECT_EQ(splatFormatAccepts(r.format, SplatStage::Detect), r.detect);
        EXPECT_EQ(splatFormatAccepts(r.format, SplatStage::Metadata), r.metadata);
        EXPECT_EQ(splatFormatAccepts(r.format, SplatStage::Build), r.build);
        EXPECT_EQ(splatFormatAccepts(r.format, SplatStage::Thumbnail), r.thumbnail);
        EXPECT_EQ(splatFormatAccepts(r.format, SplatStage::Convert), r.convert);
    }
    EXPECT_TRUE(splatAccepts("a/b/scene.SPZ", SplatStage::Thumbnail));
    EXPECT_FALSE(splatAccepts("a/b/scene.splat", SplatStage::Thumbnail));
}

TEST(splatFormat, TheExtensionListsComeFromTheTableInItsOrder) {
    EXPECT_EQ(splatExtensions(SplatStage::Detect), (Extensions{"splat", "spz", "ksplat", "sog"}));
    EXPECT_EQ(splatExtensions(SplatStage::Metadata), (Extensions{"ply", "splat", "spz", "ksplat"}));
    EXPECT_EQ(splatExtensions(SplatStage::Build), (Extensions{"ply", "splat", "spz", "ksplat", "sog"}));
    EXPECT_EQ(splatExtensions(SplatStage::Thumbnail), (Extensions{"spz", "rad"}));
    EXPECT_EQ(splatExtensions(SplatStage::Convert), (Extensions{"ply", "splat", "spz"}));

    EXPECT_EQ(splatExtensionsText(SplatStage::Convert), ".ply, .splat or .spz");
    EXPECT_EQ(splatExtensionsText(SplatStage::Build), ".ply, .splat, .spz, .ksplat or .sog");
    EXPECT_EQ(splatExtensionsText(SplatStage::Thumbnail), ".spz or .rad");
}

// The handler, the thumbnail generator and STAC read the table: nothing of their own to keep in step
TEST(splatFormat, TheHandlerAndTheThumbnailGeneratorReadTheTable) {
    const EntryHandler& handler = entryHandler(EntryType::GaussianSplat);
    EXPECT_EQ(handler.extensions(), splatExtensions(SplatStage::Detect));

    const ThumbnailGenerator* generator = handler.thumbnailGenerator();
    ASSERT_NE(generator, nullptr);
    for (const auto& row : {"ply", "splat", "spz", "ksplat", "sog", "rad", "laz", "jpg"}) {
        SCOPED_TRACE(row);
        const fs::path path = std::string("scene.") + row;
        EXPECT_EQ(generator->accepts(path), splatAccepts(path, SplatStage::Thumbnail));
        EXPECT_EQ(findThumbnailGenerator(path) == generator, splatAccepts(path, SplatStage::Thumbnail));
    }
}

TEST(splatFormat, EveryFormatOfItsOwnHasTheMediaTypeOfTheBuildManifest) {
    for (const auto format : {SplatFormat::Splat, SplatFormat::Spz, SplatFormat::Ksplat, SplatFormat::Sog,
                              SplatFormat::Rad})
        EXPECT_STREQ(splatMediaType(format), GsplatMediaType) << static_cast<int>(format);
    EXPECT_EQ(splatMediaType(SplatFormat::Ply), nullptr) << "a PLY is an ordinary PLY";
    EXPECT_EQ(splatMediaType(SplatFormat::Unknown), nullptr);
}

TEST(splatFormat, ThePropertiesOfTheFormatsKeepTheirWidth) {
    EXPECT_EQ(SplatRecordSize, 32u);
    // A PLY with more than 2^32 vertices: the count is not the width of an unsigned long
    static_assert(std::is_same_v<decltype(PlyInfo::vertexCount), uint64_t>);
}

TEST(splatFormat, AnSpzContainerIsToldFromItsFirstBytes) {
    const uint8_t gzip[] = {0x1F, 0x8B, 0x08, 0x00};
    const uint8_t ngsp[] = {'N', 'G', 'S', 'P'};
    const uint8_t other[] = {'P', 'K', 3, 4};
    EXPECT_EQ(sniffSpzContainer(gzip, sizeof(gzip)), SpzContainer::Gzip);
    EXPECT_EQ(sniffSpzContainer(gzip, 2), SpzContainer::Gzip);
    EXPECT_EQ(sniffSpzContainer(ngsp, sizeof(ngsp)), SpzContainer::Ngsp);
    EXPECT_EQ(sniffSpzContainer(ngsp, 3), SpzContainer::None) << "the magic is four bytes";
    EXPECT_EQ(sniffSpzContainer(other, sizeof(other)), SpzContainer::None);
    EXPECT_EQ(sniffSpzContainer(gzip, 1), SpzContainer::None);
    EXPECT_EQ(sniffSpzContainer(gzip, 0), SpzContainer::None);
}

TEST(splatFormat, TheContentIsSniffedForEachFormat) {
    TestArea ta(TEST_NAME);
    const auto file = [&](const std::string& name, const std::string& bytes) {
        const fs::path path = ta.getPath(name);
        writeBytes(path, bytes);
        return path;
    };

    // SPZ: gzip (v1-3) or NGSP (v4)
    EXPECT_TRUE(looksLikeSplat(file("gzip.spz", std::string("\x1F\x8B\x08\x00", 4))));
    EXPECT_TRUE(looksLikeSplat(file("ngsp.spz", "NGSP")));
    EXPECT_EQ(sniffSpzContainer(ta.getPath("gzip.spz")), SpzContainer::Gzip);
    EXPECT_EQ(sniffSpzContainer(ta.getPath("ngsp.spz")), SpzContainer::Ngsp);
    EXPECT_FALSE(looksLikeSplat(file("junk.spz", "junk")));

    // .splat: a positive whole number of 32-byte records
    EXPECT_TRUE(looksLikeSplat(file("one.splat", std::string(SplatRecordSize, 'x'))));
    EXPECT_TRUE(looksLikeSplat(file("three.splat", std::string(3 * SplatRecordSize, 'x'))));
    EXPECT_FALSE(looksLikeSplat(file("partial.splat", std::string(SplatRecordSize + 1, 'x'))));
    EXPECT_FALSE(looksLikeSplat(file("empty.splat", "")));

    // .sog is a ZIP, .rad starts with its magic
    EXPECT_TRUE(looksLikeSplat(file("bundle.sog", std::string("PK\x03\x04rest", 8))));
    EXPECT_FALSE(looksLikeSplat(file("bad.sog", "RAD0")));
    EXPECT_TRUE(looksLikeSplat(file("model.rad", "RAD0rest")));
    EXPECT_FALSE(looksLikeSplat(file("bad.rad", "PK\x03\x04")));

    // .ply: the header of a splat, not of a point cloud
    const std::string splatPly =
        "ply\nformat binary_little_endian 1.0\nelement vertex 1\nproperty float x\nproperty float y\n"
        "property float z\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\n"
        "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
        "end_header\n";
    EXPECT_TRUE(looksLikeSplat(file("splat.ply", splatPly)));
    EXPECT_FALSE(looksLikeSplat(file(
        "cloud.ply", "ply\nformat binary_little_endian 1.0\nelement vertex 1\nproperty float x\nproperty float y\n"
                     "property float z\nend_header\n")));

    // What cannot be read is not a splat, and nothing throws
    EXPECT_FALSE(looksLikeSplat(ta.getPath("missing.spz")));
    EXPECT_FALSE(looksLikeSplat(ta.getPath("missing.ply")));
    EXPECT_FALSE(looksLikeSplat(file("noextension", "NGSP")));
}

} // namespace
