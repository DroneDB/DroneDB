/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef FAKE_MODEL_TOOLS_H
#define FAKE_MODEL_TOOLS_H

// The tools of a model build (ddb-nexus, Obj2Tiles) as shell scripts that record how they were
// started, and a dataset with one model to build with them. POSIX only (the tools are sh scripts).

#ifndef _WIN32

#include <fstream>
#include <iterator>
#include <string>

#include "dbops.h"
#include "fs.h"
#include "nexus_runner.h"
#include "obj2tiles_runner.h"
#include "testarea.h"
#include "utils.h"

namespace testtools
{
    inline std::string readText(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    // An executable sh script bin/<name> in the test area
    inline fs::path writeFakeTool(TestArea &ta, const std::string &name, const std::string &body)
    {
        const fs::path path = ta.getPath("bin/" + name);
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "#!/bin/sh\n" << body;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
        return path;
    }

    // ddb-nexus records its arguments in nexusLog and writes the file named by its last one;
    // Obj2Tiles records its DOTNET_PROCESSOR_COUNT in tilesLog and writes tileset.json in its
    // output folder. The tool paths and core variables are put back when it ends.
    struct FakeModelTools
    {
        TestArea &ta;
        fs::path nexusLog, tilesLog;
        EnvVarGuard nexusPath{"DDB_NEXUS_PATH"}, tilesPath{"DDB_OBJ2TILES_PATH"}, dotnet{"DOTNET_PROCESSOR_COUNT"},
            cores{"DDB_OBJ2TILES_MAX_CORES"};

        explicit FakeModelTools(TestArea &area)
            : ta(area), nexusLog(ta.getPath("nexus.log")), tilesLog(ta.getPath("tiles.log"))
        {
            unsetEnv("DOTNET_PROCESSOR_COUNT");
            unsetEnv("DDB_OBJ2TILES_MAX_CORES");
            const auto nexus = writeFakeTool(ta, "ddb-nexus",
                                             "echo \"$@\" > '" + nexusLog.string() + "'\n"
                                             "for last; do :; done\nprintf nxz > \"$last\"\n");
            const auto tiles = writeFakeTool(ta, "Obj2Tiles",
                                             "echo \"count=$DOTNET_PROCESSOR_COUNT\" > '" + tilesLog.string() + "'\n"
                                             "printf '{}' > \"$2/tileset.json\"\n");
            setEnv("DDB_NEXUS_PATH", nexus.string());
            setEnv("DDB_OBJ2TILES_PATH", tiles.string());
            ddb::nexus::findNexusBinary(true);
            ddb::obj2tiles::findObj2TilesBinary(true);
        }

        ~FakeModelTools()
        {
            nexusPath.restore();
            tilesPath.restore();
            ddb::nexus::findNexusBinary(true);
            ddb::obj2tiles::findObj2TilesBinary(true);
        }
    };

    // A dataset with one model, a single triangle (model.obj)
    struct ModelDataset
    {
        IndexFixture index;
        explicit ModelDataset(const std::string &name) : index(name, "")
        {
            const fs::path model = index.root / "model.obj";
            std::ofstream(model) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
            ddb::addToIndex(index.db.get(), {model.string()});
        }
        std::string root() const { return index.root.string(); }
    };

} // namespace testtools

#endif

#endif // FAKE_MODEL_TOOLS_H
