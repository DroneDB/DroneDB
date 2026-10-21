/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-02: one regression test per defect found by the survey. Where a defect cannot fail
// deterministically on this platform (UB that glibc tolerates, a fallback never compiled on
// Linux) the test is a characterisation test and says so.

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"

#include "align.h"
#include "constants.h"
#include "coordstransformer.h"
#include "gdal_inc.h"
#include "geo.h"
#include "geoproject.h"
#include "databasepool.h"
#include "dbops.h"
#include "ddb.h"
#include "dsmservice.h"
#include "exceptions.h"
#include "info.h"
#include "mathutil.h"
#include "randomutil.h"
#include "sqlite_database.h"
#include "status.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "utils.h"
#include "thermal.h"
#include "tilerhelper.h"
#include "timezone.h"
#include "userprofile.h"


namespace
{
    using namespace ddb;

    // Number of GDAL datasets open in the process (all threads).
    int openDatasetCount()
    {
        GDALDatasetH *datasets = nullptr;
        int count = 0;
        GDALGetOpenDatasets(&datasets, &count);
        return count;
    }

    // Writes a small single-value GeoTIFF; geoTransform == nullptr leaves it ungeoreferenced.
    void writeGeoTiff(const fs::path &p, int bands, GDALDataType type, const double *geoTransform, int epsg)
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        ASSERT_NE(drv, nullptr);
        GDALDatasetH ds = GDALCreate(drv, p.string().c_str(), 64, 64, bands, type, nullptr);
        ASSERT_NE(ds, nullptr);
        if (geoTransform != nullptr)
        {
            GDALSetGeoTransform(ds, const_cast<double *>(geoTransform));
            OGRSpatialReference srs;
            srs.importFromEPSG(epsg);
            char *wkt = nullptr;
            srs.exportToWkt(&wkt);
            GDALSetProjection(ds, wkt);
            CPLFree(wkt);
        }
        for (int b = 1; b <= bands; b++)
            GDALFillRaster(GDALGetRasterBand(ds, b), 100.0 + b, 0.0);
        GDALClose(ds);
    }

    // Starts n threads that run fn(i) at the same moment (they spin on a shared counter).
    template <typename Fn>
    void runConcurrently(int n, Fn fn)
    {
        std::atomic<int> ready{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < n; i++)
            threads.emplace_back([&, i] {
                ready.fetch_add(1);
                while (ready.load() < n)
                    std::this_thread::yield();
                fn(i);
            });
        for (auto &t : threads)
            t.join();
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
        writeFile(root / "a.ddbfile.txt", "a");
        writeFile(root / "notes.ddb.d" / "b.txt", "b");

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
        writeFile(root / "m.obj", "mtllib m.mtl\n"
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
        setEnv(DSM_SERVICE_KEY_ENV, "from-env");
        EXPECT_EQ(dsmServiceApiKey(), "from-env");
        unsetEnv(DSM_SERVICE_KEY_ENV);
        EXPECT_EQ(dsmServiceApiKey(), "");
    }

    // Without a key the download is skipped (no request): getAltitude() catches the
    // NetException and falls back to 0, as when the service is unreachable.
    TEST(Dq02Defects, Target_DsmDownloadWithoutKeyIsSkipped)
    {
        EnvVarGuard guard(DSM_SERVICE_KEY_ENV);
        unsetEnv(DSM_SERVICE_KEY_ENV);
        EXPECT_THROW(DSMService::get()->loadFromNetwork(46.84260708333, -91.99455988889), NetException);
    }

    // A failed download threw "Failed to fetch data from <url>" with the key in the URL, and
    // getAltitude() logs that message. Port 9 (discard) on loopback refuses the connection.
    TEST(Dq02Defects, Target_DsmDownloadErrorDoesNotLeakTheKey)
    {
        const std::string key = "S3cretDsmKey0123456789";
        EnvVarGuard guard(DSM_SERVICE_KEY_ENV);
        setEnv(DSM_SERVICE_KEY_ENV, key);

        std::string message;
        try
        {
            DSMService::get()->loadFromNetwork(46.84260708333, -91.99455988889,
                                               "http://127.0.0.1:9/globaldem?west={west}&API_Key={key}");
            FAIL() << "the download from a closed port succeeded";
        }
        catch (const NetException &e)
        {
            message = e.what();
        }
        EXPECT_EQ(message.find(key), std::string::npos) << message;
        EXPECT_NE(message.find("127.0.0.1:9"), std::string::npos) << "the error should still name the service: " << message;
    }

    // A key pasted with a trailing newline or spaces went into the URL verbatim.
    TEST(Dq02Defects, Target_DsmServiceApiKeyIsTrimmed)
    {
        EnvVarGuard guard(DSM_SERVICE_KEY_ENV);
        setEnv(DSM_SERVICE_KEY_ENV, " \tk3y \r\n");
        EXPECT_EQ(dsmServiceApiKey(), "k3y");
        setEnv(DSM_SERVICE_KEY_ENV, "   ");
        EXPECT_EQ(dsmServiceApiKey(), "");
    }

    // printVersions streamed std::getenv() results, null for unset variables.
    TEST(Dq02Defects, Target_GetEnvOrUnsetNeverReturnsNull)
    {
        const char *name = "DDB_DQ02_TEST_UNSET_VARIABLE";
        EnvVarGuard guard(name);
        unsetEnv(name);
        EXPECT_EQ(utils::getEnvOrUnset(name), "<unset>");
        setEnv(name, "value");
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
    // std::rand() was called from several threads (thumbs, tiler cache cleanup, geoproject
    // temp names). Characterisation: the shared helper gives each thread its own engine. The
    // race itself needs TSan (DQ-01); the evidence is that no std::rand/rand( is left in src/.
    TEST(Dq02Defects, Target_RandomHelperGivesEachThreadItsOwnEngine)
    {
        const std::mt19937 *engines[2] = {nullptr, nullptr};
        std::thread([&] { engines[0] = &threadRandomEngine(); }).join();
        std::thread([&] { engines[1] = &threadRandomEngine(); }).join();
        EXPECT_EQ(&threadRandomEngine(), &threadRandomEngine());
        EXPECT_NE(engines[0], &threadRandomEngine());
        EXPECT_NE(engines[1], &threadRandomEngine());
        for (int i = 0; i < 1000; i++)
        {
            const int v = randomInt(3, 7);
            EXPECT_GE(v, 3);
            EXPECT_LE(v, 7);
        }
        EXPECT_TRUE(oneIn(1));
    }

    // UserProfile::get, DSMService::get and Timezone::init initialized lazily without
    // synchronization. Smoke test: 16 threads hit them at once and must agree. It cannot fail
    // deterministically (earlier tests in the process may already have initialized them, and
    // a lost race is timing-dependent); DQ-01's TSan job runs it as a race detector.
    TEST(Dq02Defects, Target_SingletonsAgreeUnderConcurrentFirstUse)
    {
        constexpr int n = 16;
        std::vector<UserProfile *> profiles(n, nullptr);
        std::vector<DSMService *> dsms(n, nullptr);
        std::vector<std::string> zones(n);
        runConcurrently(n, [&](int i) {
            profiles[i] = UserProfile::get();
            dsms[i] = DSMService::get();
            zones[i] = Timezone::lookupTimezone(46.842979, -91.993219).name();
        });
        for (int i = 0; i < n; i++)
        {
            EXPECT_EQ(profiles[i], UserProfile::get());
            EXPECT_EQ(dsms[i], DSMService::get());
            EXPECT_EQ(zones[i], "America/Chicago");
        }
    }

    // The DSMService cache had no mutex. Concurrent lookups of a cached point must all see the
    // loaded entry. Not deterministic without TSan (see above).
    TEST(Dq02Defects, Target_DsmCacheConcurrentLookups)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = ta.getPath("dsm.tif");
        const double gt[6] = {10.0, 0.01, 0.0, 20.0, 0.0, -0.01};
        writeGeoTiff(tif, 1, GDT_Float32, gt, 4326);
        ASSERT_TRUE(DSMService::get()->addGeoTIFFToCache(tif, 19.9, 10.1));

        constexpr int n = 16;
        std::vector<float> altitudes(n, 0.0f);
        runConcurrently(n, [&](int i) { altitudes[i] = DSMService::get()->getAltitude(19.9, 10.1); });
        for (float a : altitudes)
            EXPECT_FLOAT_EQ(a, 101.0f);
    }

    // toUTM/fromUTM used the OCTNewCoordinateTransformation result without a null check and
    // leaked the SRS handles on throw. The SRS failure paths cannot be reached through the
    // public API with a working PROJ, so this pins the behaviour: invalid zones throw
    // GDALException, valid ones round-trip.
    TEST(Dq02Defects, Target_UtmTransformsThrowGdalExceptionOnInvalidZone)
    {
        UTMZone bad;
        bad.zone = 99;
        bad.north = true;
        EXPECT_THROW(toUTM(46.84, -91.99, bad), GDALException);
        EXPECT_THROW(fromUTM(500000.0, 5000000.0, bad), GDALException);

        const UTMZone zone = getUTMZone(46.842979, -91.993219);
        const auto p = toUTM(46.842979, -91.993219, zone);
        const auto g = fromUTM(p, zone);
        EXPECT_NEAR(g.latitude, 46.842979, 1e-9);
        EXPECT_NEAR(g.longitude, -91.993219, 1e-9);
    }

    // CoordsTransformer copied raw OGR handles (double free on copy) and leaked its SRS
    // handles when a constructor threw (the destructor does not run). The leak is in OSR
    // objects, which GDAL cannot count; the type traits fail to compile before the fix.
    TEST(Dq02Defects, Target_CoordsTransformerIsMoveOnlyAndThrowsOnBadWkt)
    {
        static_assert(!std::is_copy_constructible_v<CoordsTransformer>);
        static_assert(!std::is_copy_assignable_v<CoordsTransformer>);
        static_assert(std::is_nothrow_move_constructible_v<CoordsTransformer>);

        EXPECT_THROW(CoordsTransformer("not a wkt", 4326), GDALException);
        EXPECT_THROW(CoordsTransformer(4326, "not a wkt"), GDALException);

        CoordsTransformer a(4326, 3857);
        CoordsTransformer b(std::move(a));
        double x = 0.0, y = 0.0;
        b.transform(&x, &y);
        EXPECT_NEAR(x, 0.0, 1e-6);
        EXPECT_NEAR(y, 0.0, 1e-6);
    }

    // DSMService::addGeoTIFFToCache leaked the dataset when the raster had no geotransform,
    // no projection or more than one band.
    TEST(Dq02Defects, Target_DsmCacheClosesTheDatasetOnInvalidRaster)
    {
        TestArea ta(TEST_NAME, true);
        const auto noGeo = ta.getPath("nogeo.tif");
        writeGeoTiff(noGeo, 1, GDT_Float32, nullptr, 0);
        const double gt[6] = {10.0, 0.01, 0.0, 20.0, 0.0, -0.01};
        const auto twoBands = ta.getPath("twobands.tif");
        writeGeoTiff(twoBands, 2, GDT_Float32, gt, 4326);

        const int before = openDatasetCount();
        EXPECT_THROW(DSMService::get()->addGeoTIFFToCache(noGeo, 0, 0), GDALException);
        EXPECT_THROW(DSMService::get()->addGeoTIFFToCache(twoBands, 0, 0), GDALException);
        EXPECT_EQ(openDatasetCount(), before);
    }

    // geoProject leaked the source dataset (and the option list) when the outsize was invalid.
    TEST(Dq02Defects, Target_GeoProjectClosesTheSourceOnInvalidOutsize)
    {
        TestArea ta(TEST_NAME);
        const auto img = ta.downloadTestAsset(
            testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");
        const auto out = ta.getPath("out.tif");

        const int before = openDatasetCount();
        EXPECT_THROW(geoProject({img.string()}, out.string(), "abc", true), InvalidArgsException);
        EXPECT_EQ(openDatasetCount(), before);
    }

    // The intermediate geoprojection dataset is now an in-memory MEM dataset instead of a
    // /vsimem GTiff with a std::rand() name that was never unlinked.
    TEST(Dq02Defects, Target_GeoProjectWritesAGeoreferencedRaster)
    {
        TestArea ta(TEST_NAME);
        const auto img = ta.downloadTestAsset(
            testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");
        const auto out = ta.getPath("projected.tif");
        if (fs::exists(out))
            fs::remove(out);

        const int before = openDatasetCount();
        geoProject({img.string()}, out.string(), "10%", true);
        EXPECT_EQ(openDatasetCount(), before);

        GDALDatasetUniquePtr ds(GDALDataset::Open(out.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_TRUE(ds);
        EXPECT_EQ(ds->GetRasterCount(), 4) << "RGB + alpha";
        ASSERT_NE(ds->GetSpatialRef(), nullptr);
        double gt[6];
        EXPECT_EQ(ds->GetGeoTransform(gt), CE_None);
        EXPECT_GT(ds->GetRasterXSize(), 0);
    }

    // alignRaster leaked both datasets when reading the common grid failed. The source is a
    // VRT whose (lazily opened) backing file is missing, so RasterIO fails after both opens.
    TEST(Dq02Defects, Target_AlignClosesDatasetsWhenTheGridReadFails)
    {
        TestArea ta(TEST_NAME, true);
        const std::string vrt =
            "<VRTDataset rasterXSize=\"64\" rasterYSize=\"64\">\n"
            "  <SRS>EPSG:32615</SRS>\n"
            "  <GeoTransform>500000, 1, 0, 4000064, 0, -1</GeoTransform>\n"
            "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
            "    <SimpleSource>\n"
            "      <SourceFilename relativeToVRT=\"1\">missing.tif</SourceFilename>\n"
            "      <SourceBand>1</SourceBand>\n"
            "      <SourceProperties RasterXSize=\"64\" RasterYSize=\"64\" DataType=\"Byte\" BlockXSize=\"64\" BlockYSize=\"64\"/>\n"
            "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"64\" ySize=\"64\"/>\n"
            "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"64\" ySize=\"64\"/>\n"
            "    </SimpleSource>\n"
            "  </VRTRasterBand>\n"
            "</VRTDataset>\n";
        const auto src = ta.getPath("src.vrt");
        const auto ref = ta.getPath("ref.vrt");
        writeFile(src, vrt);
        writeFile(ref, vrt);

        const int before = openDatasetCount();
        EXPECT_THROW(alignRaster(src.string(), ref.string(), ta.getPath("out.tif").string()), GDALException);
        EXPECT_EQ(openDatasetCount(), before);
    }

    // runTiler held the Tiler in a raw pointer, leaked (with its datasets) when the zoom
    // range or tile coordinates did not parse.
    TEST(Dq02Defects, Target_RunTilerFreesTheTilerOnBadZoomRange)
    {
        TestArea ta(TEST_NAME, true);
        const auto tif = ta.getPath("raster.tif");
        const double gt[6] = {1000000.0, 10.0, 0.0, 5000640.0, 0.0, -10.0};
        writeGeoTiff(tif, 3, GDT_Byte, gt, 3857);
        std::ostringstream out;

        const int before = openDatasetCount();
        EXPECT_THROW(TilerHelper::runTiler(tif, ta.getFolder("tiles"), 256, false, out, "text", "abc"),
                     std::invalid_argument);
        EXPECT_EQ(openDatasetCount(), before);
    }
} // namespace
