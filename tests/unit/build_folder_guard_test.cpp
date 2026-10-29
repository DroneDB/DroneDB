/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The place of a build output is the proof that libddb wrote it (raster_artefacts.h), so nothing
// that comes from a user may be written under ".ddb/build": an add refuses the ".ddb" names, the
// extraction of an archive stays in its own folder, and the content of a .3tz is extracted into
// the folder of its role, which no GDAL open reads.

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "build.h"
#include "build_layout.h"
#include "dbops.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "mzip.h"
#include "raster_artefacts.h"
#include "test.h"
#include "testarea.h"
#include "userprofile.h"
#include "utils.h"

namespace
{
    using namespace ddb;

    constexpr const char *kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    // A tileset a .3tz may hold
    const char *const kTileset =
        R"({"asset":{"version":"1.1"},"geometricError":100,)"
        R"("root":{"boundingVolume":{"region":[0.1603,0.7934,0.1606,0.7937,100,150]},"geometricError":0}})";

    std::vector<fs::path> filesBelow(const fs::path &folder)
    {
        std::vector<fs::path> files;
        if (!fs::exists(folder))
            return files;
        for (const auto &entry : fs::recursive_directory_iterator(folder))
            if (entry.is_regular_file())
                files.push_back(entry.path());
        return files;
    }

    // An add never takes a path with a ".ddb" segment, however it is spelled, and never walks into it
    TEST(BuildFolderGuard, anAddCannotPutAFileUnderTheBuildFolderIntoTheIndex)
    {
        IndexFixture fx(TEST_NAME);
        const fs::path planted = fx.root / DDB_FOLDER / DDB_BUILD_PATH / kHash / CogBuildSubfolder / CogFileName;
        writeFile(planted, "not a raster");
        writeFile(fx.root / "a.txt", "alpha");

        AddOptions options;
        options.stopOnError = false;

        const std::vector<std::string> spellings = {
            planted.string(),
            (fx.root / DDB_FOLDER / DDB_BUILD_PATH / kHash).string(),
            (fx.root / DDB_FOLDER / DDB_BUILD_PATH).string(),
            (fx.root / DDB_FOLDER).string(),
            (fx.root / "." / DDB_FOLDER / DDB_BUILD_PATH / kHash / CogBuildSubfolder / CogFileName).string(),
        };
        for (const auto &spelling : spellings)
        {
            SCOPED_TRACE(spelling);
            AddResult result;
            addToIndexEx(fx.db.get(), {spelling}, options, result);
            EXPECT_TRUE(result.entries.empty());
            EXPECT_TRUE(result.unchanged.empty());
            ASSERT_EQ(result.errors.size(), 1u);
            EXPECT_EQ(result.errors[0].code, "INDEX");
        }

        // Walking the dataset skips the folder; the other file is indexed
        AddResult walked;
        addToIndexEx(fx.db.get(), {fx.root.string()}, options, walked);
        EXPECT_TRUE(walked.errors.empty());
        ASSERT_EQ(walked.entries.size(), 1u);
        EXPECT_EQ(walked.entries[0].first.path, "a.txt");

        auto count = fx.db->query("SELECT COUNT(*) FROM entries WHERE path LIKE '%ddb%'");
        count->fetch();
        EXPECT_EQ(count->getInt(0), 0);
    }

    // The files of an archive are extracted into the folder of the role they build ("3dtiles"),
    // wherever their names look like the outputs of other roles: none is an artefact, and an
    // entry that would leave that folder refuses the whole archive before anything is written
    TEST(BuildFolderGuard, theContentOfA3tzIsNeverAnArtefact)
    {
        IndexFixture fx(TEST_NAME);
        const fs::path content = fx.ta.getFolder("content");
        writeFile(content / TilesetFileName, kTileset);
        // Named as the outputs of the roles that GDAL reads
        writeFile(content / CogBuildSubfolder / CogFileName, "not a raster");
        writeFile(content / VectorBuildSubfolder / VectorGpkgFileName, "not a database");
        writeFile(content / MvtBuildSubfolder / MvtMetadataFileName, "{}");
        writeFile(content / "cog.tif", "not a raster");
        zipFolder(io::toUtf8(content), io::toUtf8(fx.root / "model.3tz"));

        AddResult added;
        addToIndexEx(fx.db.get(), {(fx.root / "model.3tz").string()}, AddOptions(), added);
        ASSERT_EQ(added.entries.size(), 1u);
        const std::string hash = added.entries[0].first.hash;
        ASSERT_FALSE(hash.empty());

        build(fx.db.get(), "model.3tz", "", true);

        const fs::path buildRoot = fs::absolute(fx.db->buildDirectory()) / hash;
        const auto extracted = filesBelow(buildRoot);
        ASSERT_GE(extracted.size(), 5u);
        for (const auto &file : extracted)
        {
            EXPECT_FALSE(raster::isLibddbArtefact(file)) << file;
            EXPECT_THROW(raster::openArtefactRaster(file), GDALException) << file;
            EXPECT_THROW(raster::openArtefactVector(file), GDALException) << file;
        }
        // Everything is in the folder of its role; the places of the other roles are empty
        EXPECT_TRUE(fs::exists(buildRoot / Tiles3DBuildSubfolder / CogBuildSubfolder / CogFileName));
        EXPECT_FALSE(fs::exists(buildRoot / CogBuildSubfolder));
        EXPECT_FALSE(fs::exists(buildRoot / VectorBuildSubfolder));
        EXPECT_FALSE(fs::exists(buildRoot / MvtBuildSubfolder));
    }

    TEST(BuildFolderGuard, an3tzEntryThatLeavesItsFolderIsRefusedBeforeAnythingIsWritten)
    {
        IndexFixture fx(TEST_NAME);
        // Zip-Slip toward the place of the COG of the same build: from the folder the archive is
        // extracted into, one level up and into "cog"
        const fs::path ttz = fx.root / "evil.3tz";
        writeZipOfEmptyEntries(ttz, {"tileset.json", "../cog/cog.tif"});

        AddResult added;
        addToIndexEx(fx.db.get(), {ttz.string()}, AddOptions(), added);
        ASSERT_EQ(added.entries.size(), 1u);
        const std::string hash = added.entries[0].first.hash;

        EXPECT_ANY_THROW(build(fx.db.get(), "evil.3tz", "", true));

        const fs::path buildRoot = fs::absolute(fx.db->buildDirectory()) / hash;
        EXPECT_FALSE(fs::exists(buildRoot / CogBuildSubfolder));
        EXPECT_TRUE(filesBelow(buildRoot).empty());
        for (const auto &file : filesBelow(fx.db->buildDirectory()))
            ADD_FAILURE() << "written: " << file;
    }

    // The extraction of any archive can be told which names it takes
    TEST(BuildFolderGuard, anExtractionRefusesTheNamesTheCallerDoesNotAcceptBeforeWritingAnything)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path archive = ta.getPath("archive.zip");
        writeZipOfEmptyEntries(archive, {"a.txt", ".ddb/build/x.tif"});

        zip::ExtractOptions options;
        options.acceptEntry = [](const std::string &name) { return name.rfind(".ddb", 0) != 0; };
        const fs::path out = ta.getPath("out");
        EXPECT_THROW(zip::extractAll(io::toUtf8(archive), io::toUtf8(out), options), ZipException);
        EXPECT_FALSE(fs::exists(out)) << "refused before the folder was made";

        options.acceptEntry = [](const std::string &) { return true; };
        EXPECT_NO_THROW(zip::extractAll(io::toUtf8(archive), io::toUtf8(out), options));
        EXPECT_TRUE(fs::exists(out / ".ddb" / "build" / "x.tif"));
    }
} // namespace
