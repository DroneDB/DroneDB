/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Probe: libddb as a host loads it. The test binaries and the CLI contain the library
// objects (they use C++ code libddb does not export), so this is the one test that runs the
// shared library itself, through ddb.h only, as Registry does over P/Invoke: it registers the
// process, indexes a small dataset (SQLite, SpatiaLite, the metadata helper, GDAL, PROJ), reads it back,
// generates a thumbnail, builds every entry (GDAL, PDAL, the vector driver) and shuts down.
// Runs as its own process because DDBRegisterProcess initializes once per process.
//
// DDB_PROBE_INPUT_DIR: the files to index (tests/contract/inputs); DDB_PROBE_WORK_DIR: where
// the dataset is made (emptied first). Exits 0 when every step works, 1 otherwise (and says
// which step on stderr).

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "ddb.h"

namespace
{
    namespace fs = std::filesystem;

    bool fail(const std::string &step)
    {
        const char *message = DDBGetLastError();
        std::cerr << step << " failed: " << (message != nullptr ? message : "(no message)") << std::endl;
        return false;
    }

    // The JSON a call returned, freed
    std::string take(char *output)
    {
        std::string text = output != nullptr ? output : "";
        DDBFree(output);
        return text;
    }

    bool contains(const std::string &json, const std::string &text, const std::string &step)
    {
        if (json.find(text) != std::string::npos)
            return true;
        std::cerr << step << ": expected " << text << " in " << json << std::endl;
        return false;
    }

    bool prepareDataset(const fs::path &input, const fs::path &work)
    {
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work.parent_path(), ec);
        fs::copy(input, work, fs::copy_options::recursive, ec);
        if (ec)
        {
            std::cerr << "cannot copy " << input << " to " << work << ": " << ec.message() << std::endl;
            return false;
        }
        return true;
    }

    bool indexDataset(const std::string &dataset)
    {
        char *initialized = nullptr;
        if (DDBInit(dataset.c_str(), &initialized) != DDBERR_NONE)
            return fail("DDBInit");
        DDBFree(initialized);

        const char *paths[] = {dataset.c_str()};
        char *added = nullptr;
        if (DDBAdd(dataset.c_str(), paths, 1, &added, true) != DDBERR_NONE)
            return fail("DDBAdd");
        DDBFree(added);
        return true;
    }

    bool readDataset(const std::string &dataset)
    {
        const char *paths[] = {dataset.c_str()};
        char *listing = nullptr;
        if (DDBList(dataset.c_str(), paths, 1, &listing, "json", true, 0) != DDBERR_NONE)
            return fail("DDBList");
        const std::string json = take(listing);
        bool ok = true;
        for (const char *name : {"a.txt", "ortho.tif", "layer.geojson", "cloud.las", "img.jpg"})
            ok = contains(json, name, "DDBList") && ok;

        char *entry = nullptr;
        if (DDBGet(dataset.c_str(), "ortho.tif", &entry) != DDBERR_NONE)
            return fail("DDBGet");
        return contains(take(entry), "\"path\":\"ortho.tif\"", "DDBGet") && ok;
    }

    bool thumbnail(const std::string &dataset)
    {
        const std::string raster = dataset + "/ortho.tif";
        uint8_t *buffer = nullptr;
        int size = 0;
        if (DDBGenerateMemoryThumbnail(raster.c_str(), 64, &buffer, &size) != DDBERR_NONE)
            return fail("DDBGenerateMemoryThumbnail");
        const bool ok = buffer != nullptr && size > 0;
        DDBVSIFree(buffer);
        if (!ok)
            std::cerr << "DDBGenerateMemoryThumbnail returned no data" << std::endl;
        return ok;
    }

    bool build(const std::string &dataset)
    {
        if (DDBBuild(dataset.c_str(), nullptr, nullptr, false, false) != DDBERR_NONE)
            return fail("DDBBuild");
        const char *paths[] = {"ortho.tif", "layer.geojson", "cloud.las"};
        char *status = nullptr;
        if (DDBGetBuildStatus(dataset.c_str(), paths, 3, &status) != DDBERR_NONE)
            return fail("DDBGetBuildStatus");
        const std::string json = take(status);
        std::cout << "build status: " << json << std::endl;
        return !json.empty();
    }

    bool checkVersion()
    {
        const uint32_t abi = DDBGetAbiVersion();
        // The caller's build-time ABI, from the header it compiled against: the same MAJOR and a
        // MINOR at least as large
        const uint32_t built = (static_cast<uint32_t>(DDB_ABI_MAJOR) << 16) | static_cast<uint32_t>(DDB_ABI_MINOR);
        if ((abi >> 16) != DDB_ABI_MAJOR || abi < built)
        {
            std::cerr << "ABI " << abi << " does not match " << DDB_ABI_MAJOR << "." << DDB_ABI_MINOR << std::endl;
            return false;
        }
        const std::string capabilities = DDBGetCapabilities();
        return contains(capabilities, "\"DDBRegisterProcess\"", "DDBGetCapabilities") &&
               contains(capabilities, std::string("\"version\":\"") + DDB_VERSION_STRING,
                        "DDBGetCapabilities");
    }
}

int main()
{
    const char *input = std::getenv("DDB_PROBE_INPUT_DIR");
    const char *work = std::getenv("DDB_PROBE_WORK_DIR");
    if (input == nullptr || work == nullptr || *input == '\0' || *work == '\0')
    {
        std::cerr << "DDB_PROBE_INPUT_DIR and DDB_PROBE_WORK_DIR must be set" << std::endl;
        return 1;
    }
    if (!checkVersion())
        return 1;
    if (DDBRegisterProcess(false) != DDBERR_NONE)
    {
        fail("DDBRegisterProcess");
        return 1;
    }

    const fs::path dataset = fs::path(work);
    if (!prepareDataset(fs::path(input), dataset))
        return 1;
    const std::string folder = dataset.generic_string();
    if (!indexDataset(folder) || !readDataset(folder) || !thumbnail(folder) || !build(folder))
        return 1;

    if (DDBShutdown(10000) != DDBERR_NONE)
    {
        fail("DDBShutdown");
        return 1;
    }
    return 0;
}
