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
#include "raster_helper.h"
#include "raster_test_utils.h"
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

    // The ".ddb" folder is reserved however a file system may spell it: Windows and macOS open
    // ".DDB" as ".ddb", and Win32 drops trailing dots and spaces, so an upload to such a name
    // would land in the builds. An add refuses those names, a walk skips them, and on a file
    // system where they are folders of their own their files are no artefacts.
    TEST(BuildFolderGuard, anAddRefusesTheReservedFolderInAnyCaseAndWithTrailingDotsOrSpaces)
    {
        IndexFixture fx(TEST_NAME);
        writeFile(fx.root / "a.txt", "alpha");

        const std::vector<fs::path> spellings = {
            fs::path(".DDB") / DDB_BUILD_PATH / kHash / CogBuildSubfolder / CogFileName,
            fs::path(".Ddb") / "x",
            fs::path(".ddb.") / "x",
            fs::path(".ddb ") / "x",
            fs::path("sub") / ".dDb" / "x",
        };
        for (const auto &spelling : spellings)
            writeFile(fx.root / spelling, "not a raster");

        AddOptions options;
        options.stopOnError = false;
        for (const auto &spelling : spellings)
        {
            SCOPED_TRACE(spelling.generic_string());
            AddResult result;
            addToIndexEx(fx.db.get(), {(fx.root / spelling).string()}, options, result);
            EXPECT_TRUE(result.entries.empty());
            EXPECT_TRUE(result.unchanged.empty());
            ASSERT_EQ(result.errors.size(), 1u);
            EXPECT_EQ(result.errors[0].code, "INDEX");
        }

        AddResult walked;
        addToIndexEx(fx.db.get(), {fx.root.string()}, options, walked);
        EXPECT_TRUE(walked.errors.empty());
        std::vector<std::string> paths;
        for (const auto &[entry, updated] : walked.entries)
            paths.push_back(entry.path);
        std::sort(paths.begin(), paths.end());
        EXPECT_EQ(paths, (std::vector<std::string>{"a.txt", "sub"}));

        // Where ".DDB" is not the folder of the builds, what is below it was not written by libddb
        const fs::path upper = fx.root / spellings[0];
        std::error_code ec;
        if (!fs::equivalent(fx.root / ".DDB", fx.root / DDB_FOLDER, ec))
        {
            EXPECT_FALSE(raster::isLibddbArtefact(upper));
        }
    }

    // A raster helper that reads files as gdal-util does, and writes what is no raster for a COG
    class GarbageCogHelper : public raster::IRasterHelper
    {
      public:
        raster::ProbeResult probe(const fs::path &path, const raster::ProbeParams &params) override
        {
            return real_.probe(path, params);
        }
        void thumbnail(const fs::path &path, const fs::path &out, const raster::ThumbnailParams &params) override
        {
            real_.thumbnail(path, out, params);
        }
        void normalize(const fs::path &path, const fs::path &out, const json &params) override
        {
            real_.normalize(path, out, params);
        }
        void geoproject(const fs::path &path, const fs::path &out, const json &params) override
        {
            real_.geoproject(path, out, params);
        }
        void buildCog(const fs::path &, const fs::path &out, const json &, const raster::ProgressCallback &) override
        {
            writeFile(out, "II*\0 this is no TIFF");
        }
        void buildVector(const fs::path &path, const fs::path &out, const json &params,
                         const raster::ProgressCallback &progress) override
        {
            real_.buildVector(path, out, params, progress);
        }

      private:
        raster::GdalUtilHelper real_;
    };

    // The COG the helper wrote is trusted by its place once it is published: a file that does
    // not open as a raster fails the build, and nothing is published
    TEST(BuildFolderGuard, aCogOfTheHelperThatIsNoRasterIsNotPublished)
    {
        IndexFixture fx(TEST_NAME);
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        testutils::createGeoRaster(fx.root / "ortho.tif", spec);
        addToIndex(fx.db.get(), {(fx.root / "ortho.tif").string()});

        GarbageCogHelper helper;
        const raster::ScopedRasterHelper installed(helper);
        EXPECT_THROW(build(fx.db.get(), "ortho.tif", "", true), AppException);

        for (const auto &file : filesBelow(fx.root / DDB_FOLDER / DDB_BUILD_PATH))
            EXPECT_NE(file.filename(), CogFileName) << io::toUtf8(file);
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
