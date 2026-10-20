/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-02: one regression test per defect found by the survey. Where a defect cannot fail
// deterministically on this platform (UB that glibc tolerates, a fallback never compiled on
// Linux) the test is a characterisation test and says so.

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "constants.h"
#include "databasepool.h"
#include "dbops.h"
#include "ddb.h"
#include "dsmservice.h"
#include "exceptions.h"
#include "info.h"
#include "mathutil.h"
#include "sqlite_database.h"
#include "status.h"
#include "test.h"
#include "testarea.h"
#include "thermal.h"

#ifdef _WIN32
#define DDB_SETENV(name, value) _putenv_s(name, value)
#define DDB_UNSETENV(name) _putenv_s(name, "")
#else
#define DDB_SETENV(name, value) setenv(name, value, 1)
#define DDB_UNSETENV(name) unsetenv(name)
#endif

namespace
{
    using namespace ddb;

    // Saves and restores an environment variable around a test.
    struct EnvVarGuard
    {
        std::string name;
        std::optional<std::string> saved;
        explicit EnvVarGuard(const char *n) : name(n)
        {
            if (const char *v = std::getenv(n))
                saved = v;
        }
        ~EnvVarGuard()
        {
            if (saved)
                DDB_SETENV(name.c_str(), saved->c_str());
            else
                DDB_UNSETENV(name.c_str());
        }
    };

    void writeText(const fs::path &p, const std::string &content)
    {
        if (p.has_parent_path())
            fs::create_directories(p.parent_path());
        std::ofstream out(p.string(), std::ios::binary);
        out << content;
    }

    // Statement::step used to map every unlisted SQLite result code (READONLY, IOERR,
    // FULL, CORRUPT...) to "no rows", so fetch() on a failing statement looked like an empty
    // result.
    TEST(Dq02Defects, Target_StepThrowsWhenTheDatabaseIsReadOnly)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = ta.getPath("ro.sqlite");

        SqliteDatabase db;
        db.open(file.string());
        db.exec("CREATE TABLE t (x INTEGER)");
        // The connection refuses writes: sqlite3_step returns SQLITE_READONLY.
        db.exec("PRAGMA query_only = 1");

        const auto q = db.query("INSERT INTO t VALUES (1)");
        // fetch() surfaces the step result directly (execute() also resets, which reported
        // the error only by accident); before the fix it returned false ("no rows").
        try
        {
            q->fetch();
            ADD_FAILURE() << "step on a refused write must throw";
        }
        catch (const SQLException &e)
        {
            EXPECT_NE(std::string(e.what()).find("readonly"), std::string::npos) << e.what();
        }

        db.exec("PRAGMA query_only = 0");
        EXPECT_FALSE(db.query("SELECT x FROM t")->fetch()) << "the refused INSERT must not have written a row";
    }

    // statusIndex used to skip any path containing ".ddb" anywhere (including the
    // dataset root), so unindexed files such as "a.ddbfile.txt" were never reported.
    TEST(Dq02Defects, Target_StatusReportsUnindexedFilesWhoseNameContainsDdb)
    {
        TestArea ta(TEST_NAME, true);
        // The dataset root itself sits under a folder whose name contains ".ddb".
        const auto root = ta.getFolder("x.ddb-y");
        initIndex(root.string());
        writeText(root / "a.ddbfile.txt", "a");
        writeText(root / "notes.ddb.d" / "b.txt", "b");

        const auto db = ddb::open(root.string(), false);
        std::map<std::string, FileStatus> seen;
        statusIndex(db.get(), [&seen](const FileStatus s, const std::string &file) { seen[file] = s; });

        ASSERT_EQ(seen.count("a.ddbfile.txt"), 1u);
        EXPECT_EQ(seen["a.ddbfile.txt"], NotIndexed);
        ASSERT_EQ(seen.count("notes.ddb.d/b.txt"), 1u);
        EXPECT_EQ(seen["notes.ddb.d/b.txt"], NotIndexed);
        for (const auto &[file, status] : seen)
        {
            EXPECT_NE(file, DDB_FOLDER);
            EXPECT_NE(file.rfind(std::string(DDB_FOLDER) + "/", 0), 0u) << file << " is inside the .ddb folder";
        }
    }

    // addPathError (ddb.cpp) evaluated `!!p.find('*')`, always true, so every path error was
    // "FS". Helper-level test: the shared mapper used by addPathError and the add pipeline.
    TEST(Dq02Defects, Target_AddErrorCodeMapsExceptionTypes)
    {
        EXPECT_STREQ(addErrorCode(FSException("x")), "FS");
        EXPECT_STREQ(addErrorCode(GDALException("x")), "GDAL");
        EXPECT_STREQ(addErrorCode(PDALException("x")), "PDAL");
        EXPECT_STREQ(addErrorCode(JSONException("x")), "JSON");
        EXPECT_STREQ(addErrorCode(IndexException("x")), "INDEX");
        EXPECT_EQ(addErrorCode(InvalidArgsException("x")), nullptr);
        EXPECT_EQ(addErrorCode(std::runtime_error("x")), nullptr);
    }

    // DDBBuild returned DDBERR_BUILDDEPMISSING without setting the last error, so callers
    // read a stale or empty message.
    TEST(Dq02Defects, Target_BuildDepMissingSetsLastError)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        initIndex(root.string());
        // A textured OBJ whose MTL is missing: the build is deferred (BuildDepMissing).
        writeText(root / "m.obj", "mtllib m.mtl\n"
                                  "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                  "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                  "usemtl mat\n"
                                  "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n");
        {
            const auto db = ddb::open(root.string(), true);
            addToIndex(db.get(), {(root / "m.obj").string()});
        }

        // Leave a known stale message behind.
        ASSERT_EQ(DDBBuild(nullptr), DDBERR_EXCEPTION);
        const std::string stale = DDBGetLastError();

        EXPECT_EQ(DDBBuild(root.string().c_str(), "m.obj"), DDBERR_BUILDDEPMISSING);
        const std::string err = DDBGetLastError();
        EXPECT_NE(err, stale);
        EXPECT_NE(err.find("m.mtl"), std::string::npos) << err;
    }

    // pixelToGeo left geoX/geoY unwritten when the projection could not be imported.
    TEST(Dq02Defects, Target_PixelToGeoFallsBackToMapCoordsOnBadProjection)
    {
        const double gt[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
        double geoX = std::numeric_limits<double>::quiet_NaN();
        double geoY = std::numeric_limits<double>::quiet_NaN();

        pixelToGeo(2.0, 3.0, gt, "this is not a projection", geoX, geoY);

        EXPECT_DOUBLE_EQ(geoX, 12.0);
        EXPECT_DOUBLE_EQ(geoY, 17.0);
    }

    // info(stopOnError=true) rethrew with `throw e`, slicing the error to AppException.
    TEST(Dq02Defects, Target_InfoStopOnErrorRethrowsOriginalType)
    {
        TestArea ta(TEST_NAME, true);
        const auto missing = ta.getPath("does-not-exist.tif");
        std::ostringstream out;

        EXPECT_THROW(info({missing.string()}, out, "json", false, 0, "auto", false, true), FSException);
    }

    // Part 1 made Statement::step throw on SQLITE_READONLY, so a legacy (user_version 0)
    // index on a read-only file stopped opening: its schema is current, but the final
    // user_version stamp used to fail silently and now threw.
    TEST(Dq02Defects, Target_LegacyIndexOnReadOnlyFileOpens)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        initIndex(root.string());
        const auto dbFile = root / DDB_FOLDER / DDB_DATABASE_FILE;
        DatabasePool::instance().releaseConnections(root);
        {
            SqliteDatabase legacy;
            legacy.open(dbFile.string());
            legacy.setUserVersion(0);
        }

        const auto writable = fs::status(dbFile).permissions();
        fs::permissions(dbFile, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
        struct RestorePerms
        {
            fs::path p;
            fs::perms perms;
            ~RestorePerms()
            {
                DatabasePool::instance().releaseConnections(p.parent_path().parent_path());
                fs::permissions(p, perms, fs::perm_options::replace);
            }
        } restore{dbFile, writable};

        {
            SqliteDatabase probe;
            probe.open(dbFile.string());
            if (!probe.isReadOnly())
                GTEST_SKIP() << "the file is still writable (running as root?)";
        }

        std::unique_ptr<Database> db;
        ASSERT_NO_THROW(db = ddb::open(root.string(), false));
        EXPECT_TRUE(db->isReadOnly());
        EXPECT_EQ(db->getUserVersion(), 0) << "a read-only connection cannot stamp the version";
        EXPECT_TRUE(db->tableExists("entries"));
    }

    // The read-only skip must not hide real failures: on a writable index the stamp is written.
    TEST(Dq02Defects, Target_LegacyIndexOnWritableFileIsStamped)
    {
        TestArea ta(TEST_NAME, true);
        const auto root = ta.getFolder("ds");
        initIndex(root.string());
        const auto dbFile = root / DDB_FOLDER / DDB_DATABASE_FILE;
        DatabasePool::instance().releaseConnections(root);
        {
            SqliteDatabase legacy;
            legacy.open(dbFile.string());
            legacy.setUserVersion(0);
        }

        const auto db = ddb::open(root.string(), false);
        EXPECT_FALSE(db->isReadOnly());
        EXPECT_EQ(db->getUserVersion(), DDB_SCHEMA_VERSION);
    }

    // The OpenTopography key was committed in DEFAULT_DSM_SERVICE_URL. It is now read from
    // DDB_OPENTOPOGRAPHY_KEY at call time and filled into a keyless template.
    TEST(Dq02Defects, Target_DsmServiceUrlTakesTheKeyFromTheEnvironment)
    {
        const std::string tmpl = DEFAULT_DSM_SERVICE_URL;
        EXPECT_NE(tmpl.find("API_Key={key}"), std::string::npos) << "the template must not carry a key: " << tmpl;

        const auto url = buildDsmServiceUrl(tmpl, "k3y", 1.5, -2.25, 3.0, 4.0);
        ASSERT_TRUE(url.has_value());
        EXPECT_NE(url->find("west=1.500000"), std::string::npos) << *url;
        EXPECT_NE(url->find("south=-2.250000"), std::string::npos) << *url;
        EXPECT_NE(url->find("east=3.000000"), std::string::npos) << *url;
        EXPECT_NE(url->find("north=4.000000"), std::string::npos) << *url;
        EXPECT_NE(url->find("API_Key=k3y"), std::string::npos) << *url;
        EXPECT_EQ(url->find('{'), std::string::npos) << *url;
        EXPECT_FALSE(buildDsmServiceUrl(tmpl, "", 1.5, -2.25, 3.0, 4.0).has_value());

        EnvVarGuard guard(DSM_SERVICE_KEY_ENV);
        DDB_SETENV(DSM_SERVICE_KEY_ENV, "from-env");
        EXPECT_EQ(dsmServiceApiKey(), "from-env");
        DDB_UNSETENV(DSM_SERVICE_KEY_ENV);
        EXPECT_EQ(dsmServiceApiKey(), "");
    }

    // Without a key the download is skipped (no request): getAltitude() catches the
    // NetException and falls back to 0, as when the service is unreachable.
    TEST(Dq02Defects, Target_DsmDownloadWithoutKeyIsSkipped)
    {
        EnvVarGuard guard(DSM_SERVICE_KEY_ENV);
        DDB_UNSETENV(DSM_SERVICE_KEY_ENV);
        EXPECT_THROW(DSMService::get()->loadFromNetwork(46.84260708333, -91.99455988889), NetException);
    }

    // printVersions streamed std::getenv() results, null for unset variables.
    TEST(Dq02Defects, Target_GetEnvOrUnsetNeverReturnsNull)
    {
        const char *name = "DDB_DQ02_TEST_UNSET_VARIABLE";
        EnvVarGuard guard(name);
        DDB_UNSETENV(name);
        EXPECT_EQ(utils::getEnvOrUnset(name), "<unset>");
        DDB_SETENV(name, "value");
        EXPECT_EQ(utils::getEnvOrUnset(name), "value");
    }

    // Characterisation: the label tested DEBUG, which no build defines, so every build said
    // "Release". This passes in the Release build used today (it would fail in Debug before
    // the fix).
    TEST(Dq02Defects, Target_BuildInfoLabelFollowsNdebug)
    {
        const std::string info = utils::getBuildInfo();
#ifdef NDEBUG
        EXPECT_EQ(info.rfind("Release build", 0), 0u) << info;
#else
        EXPECT_EQ(info.rfind("Debug build", 0), 0u) << info;
#endif
    }

    // Characterisation: Tiler checked `ceil(log2(n) != floor(log2(n)))`, ceil of a bool. It
    // happened to give the right answer, so this cannot fail today; it pins the integer check.
    TEST(Dq02Defects, Target_TileSizePowerOfTwoCheck)
    {
        static_assert(isPowerOfTwo(1) && isPowerOfTwo(2) && isPowerOfTwo(256));
        static_assert(!isPowerOfTwo(0) && !isPowerOfTwo(-2) && !isPowerOfTwo(3) && !isPowerOfTwo(257));
        EXPECT_TRUE(isPowerOfTwo(1));
        EXPECT_TRUE(isPowerOfTwo(2));
        EXPECT_FALSE(isPowerOfTwo(3));
        EXPECT_TRUE(isPowerOfTwo(256));
        EXPECT_FALSE(isPowerOfTwo(257));
        EXPECT_TRUE(isPowerOfTwo(1 << 30));
    }

    // Characterisation: utils.h fell back to M_PI = 3.1415926535 (truncated) when <cmath>
    // did not define it (MSVC without _USE_MATH_DEFINES); glibc always defines it, so this
    // cannot fail on Linux.
    TEST(Dq02Defects, Target_PiIsFullPrecision)
    {
        static_assert(kPi == 3.14159265358979323846);
        EXPECT_EQ(kPi, std::acos(-1.0));
        EXPECT_EQ(utils::deg2rad(180.0), std::acos(-1.0));
        EXPECT_EQ(utils::rad2deg(std::acos(-1.0)), 180.0);
    }

    // Characterisation: utils::toLower/toUpper passed a plain char (negative for UTF-8
    // bytes) to std::tolower/toupper, which is undefined behaviour. glibc tolerates negative
    // values, so this passes before the fix too; MSVC debug builds assert.
    TEST(Dq02Defects, Target_CaseHelpersLeaveHighBitBytesUnchanged)
    {
        const std::string utf8 = "Caf\xc3\xa9 \xc3\x80" "BC \xe2\x82\xac" "9";  // "Café ÀBC €9"
        std::string lower = utf8;
        utils::toLower(lower);
        EXPECT_EQ(lower, "caf\xc3\xa9 \xc3\x80" "bc \xe2\x82\xac" "9");
        std::string upper = utf8;
        utils::toUpper(upper);
        EXPECT_EQ(upper, "CAF\xc3\xa9 \xc3\x80" "BC \xe2\x82\xac" "9");

        EXPECT_EQ(utils::toUpperChar('\xc3'), '\xc3');
        EXPECT_EQ(utils::toLowerChar('\x80'), '\x80');
        EXPECT_FALSE(utils::isDigitChar('\xd9'));
        EXPECT_TRUE(utils::isDigitChar('7'));

        std::string padded = "\xc2\xa0 x \xc2\xa0";  // NBSP bytes are not ASCII spaces
        utils::trim(padded);
        EXPECT_EQ(padded, "\xc2\xa0 x \xc2\xa0");
    }
} // namespace
