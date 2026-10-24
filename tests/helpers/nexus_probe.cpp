/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Nexus exit probe: a host (Registry, under dotnet) runs the Nexus build on a worker thread, and
// the process then ends with exit(). libnexus creates its QCoreApplication on the first build; a
// static one is destroyed by exit() on the main thread while it belongs to the (long gone)
// worker, which crashed the process (SIGSEGV) after the work was done. The ddb CLI builds on the
// main thread and never saw it.
//
// Usage: ddbtest_nexus_probe <main|thread> <folder>
// Builds a small textured model (a quad, OBJ + MTL + PNG) into <folder>/model.nxs, on the main
// thread or on a std::thread that is joined before main returns. Exits 0 when the build
// succeeded and its output exists, 1 otherwise; a crash while the process exits is what the
// test looks for.

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_priv.h>

#include "nxs.h"

namespace
{
    // A 64x64 RGB gradient the OBJ's material refers to: the texture path of the build reads it
    // through Qt's image readers (the plugins libnexus carries)
    bool writeTexture(const std::string &path)
    {
        GDALAllRegister();
        GDALDriver *mem = GetGDALDriverManager()->GetDriverByName("MEM");
        GDALDriver *png = GetGDALDriverManager()->GetDriverByName("PNG");
        if (mem == nullptr || png == nullptr)
            return false;
        GDALDataset *src = mem->Create("", 64, 64, 3, GDT_Byte, nullptr);
        if (src == nullptr)
            return false;
        unsigned char row[64];
        for (int band = 1; band <= 3; band++)
            for (int y = 0; y < 64; y++)
            {
                for (int x = 0; x < 64; x++)
                    row[x] = static_cast<unsigned char>((x * 4 + y * band) % 256);
                if (src->GetRasterBand(band)->RasterIO(GF_Write, 0, y, 64, 1, row, 64, 1, GDT_Byte, 0, 0) != CE_None)
                {
                    GDALClose(src);
                    return false;
                }
            }
        GDALDataset *out = png->CreateCopy(path.c_str(), src, FALSE, nullptr, nullptr, nullptr);
        GDALClose(src);
        if (out == nullptr)
            return false;
        GDALClose(out);
        return true;
    }

    bool writeModel(const std::string &folder)
    {
        if (!writeTexture(folder + "/texture.png"))
            return false;
        std::ofstream mtl(folder + "/model.mtl");
        mtl << "newmtl m\nKd 1 1 1\nmap_Kd texture.png\n";
        std::ofstream obj(folder + "/model.obj");
        obj << "mtllib model.mtl\nusemtl m\n"
               "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
               "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
        return mtl.good() && obj.good();
    }

    int build(const std::string &folder)
    {
        char error[512] = {};
        const std::string output = folder + "/model.nxs";
        const NXSErr err = nexusBuild((folder + "/model.obj").c_str(), output.c_str(), error, sizeof(error));
        if (err != NXSERR_NONE)
        {
            std::fprintf(stderr, "nexusBuild failed (%d): %s\n", static_cast<int>(err), error);
            return 1;
        }
        std::ifstream built(output, std::ios::binary | std::ios::ate);
        if (!built || built.tellg() <= 0)
        {
            std::fprintf(stderr, "nexusBuild produced no output\n");
            return 1;
        }
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const std::string mode = argv[1];
    const std::string folder = argv[2];
    if (!writeModel(folder))
    {
        std::fprintf(stderr, "could not write the model in %s\n", folder.c_str());
        return 1;
    }

    int result = 1;
    if (mode == "main")
        result = build(folder);
    else if (mode == "thread")
        std::thread([&] { result = build(folder); }).join();
    else
        return 2;

    std::fprintf(stdout, "nexus probe %s\n", result == 0 ? "ok" : "failed");
    // Returning from main runs the static destructors in exit(): where the crash was
    return result;
}
