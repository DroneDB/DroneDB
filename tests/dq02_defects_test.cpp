/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-02 (first half): one regression test per defect found by the survey.

#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "dbops.h"
#include "ddb.h"
#include "exceptions.h"
#include "info.h"
#include "sqlite_database.h"
#include "status.h"
#include "test.h"
#include "testarea.h"
#include "thermal.h"

namespace
{
    using namespace ddb;

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
} // namespace
