/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// ddb-nexus has no GUI toolkit behind it: it reads the textures of a model with image libraries
// of its own and the faces of a PLY with a parser of its own. These tests build small models
// made inside the test, one texture file format at a time, and read the header of what was
// written: the formats the textures come in, the polygons of a PLY, an OBJ with one-dimensional
// texture coordinates, and the failures that must leave no file behind.

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
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;

    // The part of the Nexus header these tests read: the magic, the sizes and the texture table
    struct NxsHeader
    {
        char magic[4];
        uint64_t nvert;
        uint64_t nface;
        uint32_t nNodes;
        uint32_t nPatches;
        uint32_t nTextures;
    };

    NxsHeader readHeader(const std::string &path)
    {
        std::ifstream in(io::fromUtf8(path), std::ios::binary);
        std::vector<char> b(88);
        in.read(b.data(), static_cast<std::streamsize>(b.size()));
        EXPECT_EQ(in.gcount(), 88) << path;
        NxsHeader h{};
        std::memcpy(h.magic, b.data(), 4);
        std::memcpy(&h.nvert, b.data() + 8, 8);
        std::memcpy(&h.nface, b.data() + 16, 8);
        std::memcpy(&h.nNodes, b.data() + 60, 4);
        std::memcpy(&h.nPatches, b.data() + 64, 4);
        std::memcpy(&h.nTextures, b.data() + 68, 4);
        return h;
    }

    // The first texture of the file is a JPEG, whatever format the model's texture came in
    void expectFirstTextureIsJpeg(const std::string &path, const NxsHeader &h)
    {
        std::ifstream in(io::fromUtf8(path), std::ios::binary);
        uint32_t offset = 0;
        in.seekg(88 + static_cast<std::streamoff>(h.nNodes) * 44 + static_cast<std::streamoff>(h.nPatches) * 12);
        in.read(reinterpret_cast<char *>(&offset), 4);
        in.seekg(static_cast<std::streamoff>(offset) * 256);
        unsigned char soi[2] = {0, 0};
        in.read(reinterpret_cast<char *>(soi), 2);
        EXPECT_TRUE(in.good());
        EXPECT_EQ(soi[0], 0xff);
        EXPECT_EQ(soi[1], 0xd8);
    }

    void expectNxsMagic(const NxsHeader &h)
    {
        // 'N' 'x' 's' ' ' in a little-endian word
        uint32_t word;
        std::memcpy(&word, h.magic, 4);
        EXPECT_EQ(word, 0x4e787320u);
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

        const NxsHeader h = readHeader(io::toUtf8(out));
        expectNxsMagic(h);
        EXPECT_EQ(h.nface, 2u);
        EXPECT_EQ(h.nvert, 4u);
        ASSERT_GE(h.nTextures, 1u) << "no texture in the file";
        expectFirstTextureIsJpeg(io::toUtf8(out), h);
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
        const NxsHeader h = readHeader(io::toUtf8(out));
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
        const NxsHeader h = readHeader(io::toUtf8(out));
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
        const NxsHeader quad = readHeader(io::toUtf8(ta.getPath("quad.nxs")));
        const NxsHeader tris = readHeader(io::toUtf8(ta.getPath("tris.nxs")));
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
} // namespace
