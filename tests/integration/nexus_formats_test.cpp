/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// ddb-nexus has no GUI toolkit behind it: it reads the textures of a model with image libraries
// of its own and the faces of a PLY with a parser of its own. These tests build small models
// made inside the test, one texture file format at a time, and read the header of what was
// written: the formats the textures come in (the node textures written are WebP whatever they
// were), the polygons of a PLY, an OBJ with one-dimensional texture coordinates, and the
// failures that must leave no file behind.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "3d.h"
#include "exceptions.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "nexus_file_support.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    using testutils::NexusFile;
    using testutils::readNexusFile;

    void expectNxsMagic(const NexusFile &h)
    {
        // 'N' 'x' 's' ' ' in a little-endian word
        EXPECT_EQ(h.magic, 0x4e787320u);
    }

    // A 16x16 picture with a gradient, as the bands the format being written takes
    enum class Pixels { Rgb, Rgba, Gray, Paletted };

    // Writes the picture with the GDAL driver; false when the driver is not in this build
    bool writeTexture(const fs::path &file, const char *driverName, Pixels kind)
    {
        GDALAllRegister();
        GDALDriverH mem = GDALGetDriverByName("MEM");
        GDALDriverH out = GDALGetDriverByName(driverName);
        if (mem == nullptr || out == nullptr)
            return false;

        const int size = 16;
        const int bands = kind == Pixels::Rgb ? 3 : kind == Pixels::Rgba ? 4 : 1;
        GDALDatasetH src = GDALCreate(mem, "", size, size, bands, GDT_Byte, nullptr);
        if (src == nullptr)
            return false;
        std::vector<uint8_t> row(size);
        for (int b = 0; b < bands; b++)
        {
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    row[x] = static_cast<uint8_t>(kind == Pixels::Paletted ? (x / 4 + 4 * (y / 4))
                                                                            : (x * 16 + y * 3 + b * 40) & 0xff);
                    if (kind == Pixels::Rgba && b == 3)
                        row[x] = 200;
                }
                if (GDALRasterIO(GDALGetRasterBand(src, b + 1), GF_Write, 0, y, size, 1, row.data(), size, 1, GDT_Byte, 0, 0) != CE_None)
                {
                    GDALClose(src);
                    return false;
                }
            }
            if (kind == Pixels::Rgb || kind == Pixels::Rgba)
                GDALSetRasterColorInterpretation(GDALGetRasterBand(src, b + 1),
                                                 b == 0 ? GCI_RedBand : b == 1 ? GCI_GreenBand : b == 2 ? GCI_BlueBand : GCI_AlphaBand);
        }
        if (kind == Pixels::Paletted)
        {
            GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
            for (int i = 0; i < 16; i++)
            {
                GDALColorEntry e = {static_cast<short>(i * 16), static_cast<short>(255 - i * 16), static_cast<short>(i * 8), 255};
                GDALSetColorEntry(table, i, &e);
            }
            GDALSetRasterColorTable(GDALGetRasterBand(src, 1), table);
            GDALDestroyColorTable(table);
        }

        const std::string name = io::toUtf8(file);
        GDALDatasetH dst = GDALCreateCopy(out, name.c_str(), src, FALSE, nullptr, nullptr, nullptr);
        GDALClose(src);
        if (dst == nullptr)
            return false;
        GDALClose(dst);
        std::error_code ec;
        return fs::exists(file, ec);
    }

    // Two triangles with texture coordinates, and the material that names the texture file
    void writeTexturedModel(const fs::path &folder, const std::string &texture, const char *uv = nullptr)
    {
        std::ofstream(folder / "model.obj")
            << "mtllib model.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
            << (uv != nullptr ? uv : "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n")
            << "usemtl m\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
        std::ofstream(folder / "model.mtl") << "newmtl m\nKd 1 1 1\nmap_Kd " << texture << "\n";
    }

    std::string build(const fs::path &model, const fs::path &out)
    {
        return buildNexus(io::toUtf8(model), io::toUtf8(out), true);
    }

    struct TextureCase
    {
        const char *name;
        const char *driver;
        const char *extension;
        Pixels pixels;
    };

    class NexusTextureFormat : public ::testing::TestWithParam<TextureCase>
    {
    };

    TEST_P(NexusTextureFormat, theTextureEndsUpInTheFile)
    {
        const TextureCase &c = GetParam();
        TestArea ta(TEST_NAME, true);
        const std::string textureName = std::string("texture.") + c.extension;
        if (!writeTexture(ta.getPath(textureName), c.driver, c.pixels))
            GTEST_SKIP() << "the GDAL driver " << c.driver << " cannot write this picture";
        writeTexturedModel(ta.getPath("."), textureName);

        const fs::path out = ta.getPath("model.nxs");
        ASSERT_NO_THROW(build(ta.getPath("model.obj"), out));

        const NexusFile h = readNexusFile(out);
        expectNxsMagic(h);
        EXPECT_EQ(h.nface, 2u);
        EXPECT_EQ(h.nvert, 4u);
        ASSERT_GE(h.nTextures, 1u) << "no texture in the file";
        ASSERT_FALSE(h.textures.empty());
        EXPECT_TRUE(testutils::isWebp(h.textures.front()));
    }

    INSTANTIATE_TEST_SUITE_P(Formats, NexusTextureFormat,
                             ::testing::Values(TextureCase{"jpeg", "JPEG", "jpg", Pixels::Rgb},
                                               TextureCase{"pngRgb", "PNG", "png", Pixels::Rgb},
                                               TextureCase{"pngRgba", "PNG", "png", Pixels::Rgba},
                                               TextureCase{"pngPaletted", "PNG", "png", Pixels::Paletted},
                                               TextureCase{"pngGray", "PNG", "png", Pixels::Gray},
                                               TextureCase{"tiff", "GTiff", "tif", Pixels::Rgb},
                                               TextureCase{"webp", "WEBP", "webp", Pixels::Rgb},
                                               TextureCase{"bmp", "BMP", "bmp", Pixels::Paletted},
                                               TextureCase{"gif", "GIF", "gif", Pixels::Paletted}),
                             [](const ::testing::TestParamInfo<TextureCase> &i) { return std::string(i.param.name); });

    TEST(NexusFormats, aModelWithoutTexturesHasNone)
    {
        TestArea ta(TEST_NAME, true);
        std::ofstream(ta.getPath("model.obj")) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        const fs::path out = ta.getPath("model.nxs");
        ASSERT_NO_THROW(build(ta.getPath("model.obj"), out));
        const NexusFile h = readNexusFile(out);
        expectNxsMagic(h);
        EXPECT_EQ(h.nface, 1u);
        EXPECT_EQ(h.nTextures, 0u);
    }

    TEST(NexusFormats, textureCoordinatesWithOnlyUAreAccepted)
    {
        TestArea ta(TEST_NAME, true);
        if (!writeTexture(ta.getPath("texture.png"), "PNG", Pixels::Rgb))
            GTEST_SKIP() << "no PNG driver";
        writeTexturedModel(ta.getPath("."), "texture.png", "vt 0\nvt 1\nvt 1\nvt 0\n");
        const fs::path out = ta.getPath("model.nxs");
        ASSERT_NO_THROW(build(ta.getPath("model.obj"), out));
        const NexusFile h = readNexusFile(out);
        expectNxsMagic(h);
        EXPECT_EQ(h.nface, 2u);
    }

    // The same square surface, once as a quad and once as two triangles
    std::string plyHeader(int faces)
    {
        std::ostringstream s;
        s << "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
          << "element face " << faces << "\nproperty list uchar int vertex_indices\nend_header\n"
          << "0 0 0\n1 0 0\n1 1 0\n0 1 0\n";
        return s.str();
    }

    TEST(NexusFormats, aPlyQuadHasTheFacesOfTwoTriangles)
    {
        TestArea ta(TEST_NAME, true);
        std::ofstream(ta.getPath("quad.ply")) << plyHeader(1) << "4 0 1 2 3\n";
        std::ofstream(ta.getPath("tris.ply")) << plyHeader(2) << "3 0 1 2\n3 0 2 3\n";

        ASSERT_NO_THROW(build(ta.getPath("quad.ply"), ta.getPath("quad.nxs")));
        ASSERT_NO_THROW(build(ta.getPath("tris.ply"), ta.getPath("tris.nxs")));
        const NexusFile quad = readNexusFile(ta.getPath("quad.nxs"));
        const NexusFile tris = readNexusFile(ta.getPath("tris.nxs"));
        expectNxsMagic(quad);
        EXPECT_EQ(quad.nface, 2u);
        EXPECT_EQ(quad.nface, tris.nface);
        EXPECT_EQ(quad.nvert, tris.nvert);
    }

    // What a failed build leaves in the output folder
    std::vector<std::string> filesIn(const fs::path &folder)
    {
        std::vector<std::string> names;
        for (const auto &e : fs::directory_iterator(folder))
            names.push_back(e.path().filename().string());
        return names;
    }

    void expectNothingLeftBehind(const fs::path &folder)
    {
        for (const std::string &n : filesIn(folder))
        {
            EXPECT_EQ(n.find(".part"), std::string::npos) << n;
            EXPECT_EQ(n.find(".tmp"), std::string::npos) << n;
            EXPECT_EQ(n.find(".nxs"), std::string::npos) << n;
            EXPECT_EQ(n.find(".nxz"), std::string::npos) << n;
        }
    }

    TEST(NexusFormats, aPlyFaceWithAThousandIndicesFailsAndLeavesNoFile)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path in = ta.getPath("in/big.ply");
        fs::create_directories(in.parent_path());
        const fs::path outDir = ta.getPath("out");
        fs::create_directories(outDir);
        {
            std::ofstream f(in);
            f << "ply\nformat ascii 1.0\nelement vertex 1000\nproperty float x\nproperty float y\nproperty float z\n"
              << "element face 1\nproperty list ushort int vertex_indices\nend_header\n";
            for (int i = 0; i < 1000; i++)
                f << i << " " << (i * 7 % 13) << " 0\n";
            f << "1000";
            for (int i = 0; i < 1000; i++)
                f << " " << i;
            f << "\n";
        }
        try
        {
            build(in, outDir / "big.nxs");
            FAIL() << "the build should have failed";
        }
        catch (const AppException &e)
        {
            EXPECT_NE(std::string(e.what()).find("face"), std::string::npos) << e.what();
        }
        expectNothingLeftBehind(outDir);
    }

    TEST(NexusFormats, anObjWithAGarbageFaceFailsCleanly)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path outDir = ta.getPath("out");
        fs::create_directories(outDir);
        std::ofstream(ta.getPath("bad.obj")) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 two 99999\nf -77 0 3\n";
        EXPECT_THROW(build(ta.getPath("bad.obj"), outDir / "bad.nxs"), AppException);
        expectNothingLeftBehind(outDir);
    }

    TEST(NexusFormats, coincidentTrianglesFailWithTooSmallToDivideAndLeaveNoFile)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path outDir = ta.getPath("out");
        fs::create_directories(outDir);
        {
            std::ofstream f(ta.getPath("same.obj"));
            f << "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
            for (int i = 0; i < 100000; i++)
                f << "f 1 2 3\n";
        }
        const auto start = std::chrono::steady_clock::now();
        try
        {
            build(ta.getPath("same.obj"), outDir / "same.nxs");
            FAIL() << "the build should have failed";
        }
        catch (const AppException &e)
        {
            EXPECT_NE(std::string(e.what()).find("too small to divide"), std::string::npos) << e.what();
        }
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_LT(seconds, 10.0);
        expectNothingLeftBehind(outDir);
        for (const std::string &n : filesIn(outDir))
            EXPECT_EQ(n.find("nexus_cache"), std::string::npos) << n;
    }

    // A 24-bit BMP, written byte by byte: rows padded to four bytes and stored bottom-up
    void writeBmp(const fs::path &file, int width, int height)
    {
        const int stride = (width * 3 + 3) / 4 * 4;
        const uint32_t dataSize = static_cast<uint32_t>(stride) * static_cast<uint32_t>(height);
        std::vector<unsigned char> b(54 + dataSize, 0);
        auto put32 = [&b](size_t at, uint32_t v) {
            for (int i = 0; i < 4; i++)
                b[at + i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
        };
        b[0] = 'B';
        b[1] = 'M';
        put32(2, 54 + dataSize);
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<uint32_t>(width));
        put32(22, static_cast<uint32_t>(height));
        b[26] = 1;
        b[28] = 24;
        put32(34, dataSize);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                const size_t at = 54 + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
                b[at] = static_cast<unsigned char>(x & 0xff);
                b[at + 1] = static_cast<unsigned char>(y * 30);
                b[at + 2] = static_cast<unsigned char>((x >> 8) * 20);
            }
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
    }

    TEST(NexusFormats, aTextureThatCannotBePackedBuildsWithoutTextures)
    {
        TestArea ta(TEST_NAME, true);
        writeBmp(ta.getPath("wide.bmp"), 20000, 8);
        writeTexturedModel(ta.getPath("."), "wide.bmp", "vt 0 0\nvt 0.99 0\nvt 0.99 0.99\nvt 0 0.99\n");
        const fs::path out = ta.getPath("model.nxs");
        // The warning "textures could not be packed" goes to the stderr of ddb-nexus, which libddb
        // does not surface when the build succeeds: only the outcome is asserted here
        ASSERT_NO_THROW(build(ta.getPath("model.obj"), out));
        const NexusFile h = readNexusFile(out);
        expectNxsMagic(h);
        EXPECT_EQ(h.nface, 2u);
        EXPECT_EQ(h.nTextures, 0u);
    }

    // A binary little-endian PLY of the same square as two triangles; the face list counts with
    // `countType` and indexes with `indexType`, each one of uchar, ushort, int, uint
    template <typename T> void putLe(std::string &s, T v)
    {
        for (size_t i = 0; i < sizeof(T); i++)
            s.push_back(static_cast<char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xff));
    }

    template <typename Count, typename Index>
    void writeBinaryPly(const fs::path &file, const char *countType, const char *indexType)
    {
        std::string s = std::string("ply\nformat binary_little_endian 1.0\nelement vertex 4\n"
                                    "property float x\nproperty float y\nproperty float z\n"
                                    "element face 2\nproperty list ") +
                        countType + " " + indexType + " vertex_indices\nend_header\n";
        const float xyz[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        for (const auto &v : xyz)
            for (float c : v)
            {
                uint32_t bits;
                std::memcpy(&bits, &c, 4);
                putLe(s, bits);
            }
        const int faces[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto &f : faces)
        {
            putLe(s, static_cast<Count>(3));
            for (int i : f)
                putLe(s, static_cast<Index>(i));
        }
        std::ofstream o(file, std::ios::binary);
        o.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    TEST(NexusFormats, aPlyFaceListWithAnyCountAndIndexTypeBuildsTheSameModel)
    {
        TestArea ta(TEST_NAME, true);
        writeBinaryPly<uint8_t, int32_t>(ta.getPath("uchar_int.ply"), "uchar", "int");
        writeBinaryPly<int32_t, int32_t>(ta.getPath("int_int.ply"), "int", "int");
        writeBinaryPly<uint16_t, uint32_t>(ta.getPath("ushort_uint.ply"), "ushort", "uint");

        ASSERT_NO_THROW(build(ta.getPath("uchar_int.ply"), ta.getPath("uchar_int.nxs")));
        const NexusFile reference = readNexusFile(ta.getPath("uchar_int.nxs"));
        expectNxsMagic(reference);
        EXPECT_EQ(reference.nface, 2u);
        EXPECT_EQ(reference.nvert, 4u);

        for (const char *name : {"int_int", "ushort_uint"})
        {
            ASSERT_NO_THROW(build(ta.getPath(std::string(name) + ".ply"), ta.getPath(std::string(name) + ".nxs"))) << name;
            const NexusFile h = readNexusFile(ta.getPath(std::string(name) + ".nxs"));
            expectNxsMagic(h);
            EXPECT_EQ(h.nface, reference.nface) << name;
            EXPECT_EQ(h.nvert, reference.nvert) << name;
        }
    }

    TEST(NexusFormats, aPlyWithAVeryLongTextureFileCommentDoesNotCrash)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path outDir = ta.getPath("out");
        fs::create_directories(outDir);
        {
            std::ofstream f(ta.getPath("long.ply"));
            f << "ply\nformat ascii 1.0\ncomment TextureFile " << std::string(480, 'a') << ".png\n"
              << "element vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
              << "element face 2\nproperty list uchar int vertex_indices\nend_header\n"
              << "0 0 0\n1 0 0\n1 1 0\n0 1 0\n3 0 1 2\n3 0 2 3\n";
        }
        try
        {
            build(ta.getPath("long.ply"), outDir / "long.nxs");
        }
        catch (const AppException &e)
        {
            const std::string m = e.what();
            for (const char *bad : {"signal", "crash", "Segmentation", "SIGSEGV", "SIGABRT"})
                EXPECT_EQ(m.find(bad), std::string::npos) << m;
        }
    }
} // namespace
