/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The error contract of the C API (new in 2.0).
//
//  - the last error is per thread, whole (no length limit), cleared on entry of every call and
//    set for every code but DDBERR_NONE; the exempt accessors never touch it;
//  - one mapper (c_api_util.h) turns every exception into a code: the exports of 1.x keep
//    the codes they always returned (RETURN-CODE FREEZE), the exports added in 2.0 also
//    return DDBERR_INVALIDARGS and DDBERR_RETRYABLE; DDBGetLastErrorReason says why a failure
//    is retryable, whatever the code;
//  - outputs are written only on DDBERR_NONE, and a buffer of a failed call is freed;
//  - a failed DDBRegisterProcess is final (sticky);
//  - paths cross the C API as UTF-8, whatever the locale.

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#include "build_layout.h"
#include "buildlock.h"
#include "c_api_util.h"
#include "ddb.h"
#include "entry_types.h"
#include "exifeditor.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "delta_apply.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

// Copies a string returned by the library and frees it with DDBFree
std::string take(char* s) {
    std::string copy = s != nullptr ? s : "";
    DDBFree(s);
    return copy;
}

// A pointer an output must keep when the call fails
char* const kUntouched = reinterpret_cast<char*>(static_cast<uintptr_t>(0x1));

// A dataset with a.txt ("alpha"), closed
struct Dataset {
    std::filesystem::path root;
    explicit Dataset(const std::string& name) : root(makeIndex(name)) {}
    std::string path() const { return root.string(); }
};

// ------------------------------------------------------------------ the last error

// The message was cut at 254 bytes, possibly inside a UTF-8 sequence
TEST(LastError, isWholeWithoutLengthLimit) {
    const Dataset ds(TEST_NAME);
    std::string entry;
    for (int i = 0; i < 150; i++)
        entry += "\xC3\xA8";  // "è", two bytes each: a cut at an odd length breaks one
    entry += "/missing.txt";

    char* out = kUntouched;
    EXPECT_EQ(DDBGet(ds.path().c_str(), entry.c_str(), &out), DDBERR_NOTFOUND);
    EXPECT_EQ(std::string(DDBGetLastError()), "No entry " + entry);
    EXPECT_GT(std::string(DDBGetLastError()).size(), 255u);
    EXPECT_EQ(out, kUntouched);
}

// The message of a failure stayed after later successful calls
TEST(LastError, isClearedOnEntryOfEveryCall) {
    const Dataset ds(TEST_NAME);
    char* out = nullptr;
    ASSERT_EQ(DDBInit(nullptr, &out), DDBERR_EXCEPTION);
    ASSERT_STRNE(DDBGetLastError(), "");

    ASSERT_DDB_OK(DDBGetTag(ds.path().c_str(), &out));
    take(out);
    EXPECT_STREQ(DDBGetLastError(), "");
    EXPECT_STREQ(DDBGetLastErrorReason(), "");

    // A failure replaces the message of an earlier failure
    ASSERT_EQ(DDBGet(ds.path().c_str(), "nope.txt", &out), DDBERR_NOTFOUND);
    EXPECT_STREQ(DDBGetLastError(), "No entry nope.txt");
}

TEST(LastError, exemptAccessorsKeepIt) {
    char* out = nullptr;
    ASSERT_EQ(DDBInit(nullptr, &out), DDBERR_EXCEPTION);
    const std::string error = DDBGetLastError();
    ASSERT_FALSE(error.empty());

    EXPECT_EQ(DDBFree(nullptr), DDBERR_NONE);
    EXPECT_EQ(DDBVSIFree(nullptr), DDBERR_NONE);
    EXPECT_NE(DDBGetVersion(), nullptr);
    EXPECT_NE(DDBGetAbiVersion(), 0u);
    EXPECT_NE(DDBGetCapabilities(), nullptr);
    EXPECT_STREQ(DDBGetLastErrorReason(), "");
    EXPECT_EQ(DDBGetLastError(), error);
}

TEST(LastError, belongsToTheCallingThread) {
    char* out = nullptr;
    ASSERT_EQ(DDBInit(nullptr, &out), DDBERR_EXCEPTION);
    const std::string mine = DDBGetLastError();

    std::string theirs;
    std::thread([&theirs] {
        // A new thread starts without an error, and its failure stays its own
        theirs = DDBGetLastError();
        char* o = nullptr;
        DDBGet(nullptr, nullptr, &o);
    }).join();
    EXPECT_EQ(theirs, "");
    EXPECT_EQ(DDBGetLastError(), mine);
}

// Deprecated, kept exported (new in 2.0): replaces the message, clears the reason; NULL clears
TEST(LastError, setLastErrorIsDeprecatedButWorks) {
    DDBSetLastError("set by the caller");
    EXPECT_STREQ(DDBGetLastError(), "set by the caller");
    EXPECT_STREQ(DDBGetLastErrorReason(), "");
    DDBSetLastError(nullptr);
    EXPECT_STREQ(DDBGetLastError(), "");
}

// ------------------------------------------------------------------ the mapper: code table

struct Expected {
    DDBErr frozen;     // the code of an export of 1.x (kFrozenCodes)
    DDBErr newExport;  // the code of an export new in 2.0 (kNewExportCodes)
    std::string message;
    std::string reason;
};

template <typename Throw>
void expectMapping(const char* what, Throw&& thrower, const Expected& expected) {
    SCOPED_TRACE(what);
    EXPECT_EQ(capi::guarded(thrower), expected.frozen);
    EXPECT_EQ(DDBGetLastError(), expected.message);
    EXPECT_EQ(DDBGetLastErrorReason(), expected.reason);

    EXPECT_EQ(capi::guarded<capi::kNewExportCodes>(thrower), expected.newExport);
    EXPECT_EQ(DDBGetLastError(), expected.message);
    EXPECT_EQ(DDBGetLastErrorReason(), expected.reason);
}

// RETURN-CODE FREEZE: every exception an export can throw, and the code each kind of export
// returns for it
TEST(ErrorMapper, codeTable) {
    expectMapping("DBBusyException", [] { throw DBBusyException("busy"); },
                  {DDBERR_BUSY, DDBERR_BUSY, "busy", ""});
    expectMapping("EntryNotFoundException", [] { throw EntryNotFoundException("no entry"); },
                  {DDBERR_EXCEPTION, DDBERR_NOTFOUND, "no entry", ""});
    expectMapping("BuildDepMissingException", [] { throw BuildDepMissingException("dep"); },
                  {DDBERR_EXCEPTION, DDBERR_BUILDDEPMISSING, "dep", ""});
    expectMapping("BuildInProgressException", [] { throw BuildInProgressException("locked"); },
                  {DDBERR_EXCEPTION, DDBERR_BUILDINPROGRESS, "locked", ""});
    expectMapping("CanceledException", [] { throw CanceledException("stop"); },
                  {DDBERR_EXCEPTION, DDBERR_CANCELED, "stop", ""});
    // Every export returns SHUTDOWN once DDBShutdown started (new in 2.0)
    expectMapping("ShutdownException", [] { throw ShutdownException("shutting down"); },
                  {DDBERR_SHUTDOWN, DDBERR_SHUTDOWN, "shutting down", ""});
    expectMapping("InvalidArgsException", [] { throw InvalidArgsException("bad"); },
                  {DDBERR_EXCEPTION, DDBERR_INVALIDARGS, "bad", ""});
    // A refused entry path is INVALIDARGS in every export, the exports of 1.x included
    expectMapping("InvalidPathException", [] { throw InvalidPathException("Invalid path 'x': traversal"); },
                  {DDBERR_INVALIDARGS, DDBERR_INVALIDARGS, "Invalid path 'x': traversal", ""});
    // So is every other refused argument: a raster query of a place without data, a detection
    // that finds no stockpile
    expectMapping("RefusedArgumentException", [] { throw RefusedArgumentException("window too small"); },
                  {DDBERR_INVALIDARGS, DDBERR_INVALIDARGS, "window too small", ""});
    expectMapping("NoRasterDataException", [] { throw NoRasterDataException("No valid data in the area"); },
                  {DDBERR_INVALIDARGS, DDBERR_INVALIDARGS, "No valid data in the area", ""});
    expectMapping("NoStockpileException", [] { throw NoStockpileException("No stockpile detected"); },
                  {DDBERR_INVALIDARGS, DDBERR_INVALIDARGS, "No stockpile detected", ""});
    expectMapping("RetryableBuildException(LowDiskSpace)",
                  [] { throw RetryableBuildException("disk", RetryableBuildReason::LowDiskSpace); },
                  {DDBERR_EXCEPTION, DDBERR_RETRYABLE, "disk", "lowDiskSpace"});
    expectMapping("RetryableBuildException(NoSpaceOnDevice)",
                  [] { throw RetryableBuildException("full", RetryableBuildReason::NoSpaceOnDevice); },
                  {DDBERR_EXCEPTION, DDBERR_RETRYABLE, "full", "noSpaceOnDevice"});
    expectMapping("RetryableBuildException(IdleTimeout)",
                  [] { throw RetryableBuildException("idle", RetryableBuildReason::IdleTimeout); },
                  {DDBERR_EXCEPTION, DDBERR_RETRYABLE, "idle", "idleTimeout"});
    expectMapping("BuildLockPermissionException", [] { throw BuildLockPermissionException("perm"); },
                  {DDBERR_EXCEPTION, DDBERR_EXCEPTION, "perm", ""});
    expectMapping("FSException", [] { throw FSException("fs"); },
                  {DDBERR_EXCEPTION, DDBERR_EXCEPTION, "fs", ""});
    expectMapping("std::bad_alloc", [] { throw std::bad_alloc(); },
                  {DDBERR_EXCEPTION, DDBERR_EXCEPTION, "Out of memory", ""});
    expectMapping("std::runtime_error", [] { throw std::runtime_error("std"); },
                  {DDBERR_EXCEPTION, DDBERR_EXCEPTION, "std", ""});
    expectMapping("not an exception", [] { throw 42; },
                  {DDBERR_EXCEPTION, DDBERR_EXCEPTION, "Unknown error occurred", ""});
}

// Every code but NONE has a message, also when the exception has none
TEST(ErrorMapper, defaultMessages) {
    expectMapping("AppException without a message", [] { throw AppException(""); },
                  {DDBERR_EXCEPTION, DDBERR_EXCEPTION, "Unknown error occurred", ""});
    expectMapping("BuildDepMissingException without a message", [] { throw BuildDepMissingException(""); },
                  {DDBERR_EXCEPTION, DDBERR_BUILDDEPMISSING, "Build skipped: a dependency is missing", ""});
    expectMapping("CanceledException without a message", [] { throw CanceledException(""); },
                  {DDBERR_EXCEPTION, DDBERR_CANCELED, "Canceled by the progress callback", ""});
}

TEST(ErrorMapper, successClearsAndReturnsNone) {
    DDBSetLastError("stale");
    EXPECT_EQ(capi::guarded([] {}), DDBERR_NONE);
    EXPECT_STREQ(DDBGetLastError(), "");
}

// The exports with a code of their own in 1.x, and the frozen EXCEPTION of the others
TEST(ErrorMapper, frozenCodesOfTheExports) {
    const Dataset ds(TEST_NAME);
    char* out = kUntouched;

    // DDBGet: NOTFOUND (1.x); DDBStac: EXCEPTION for the same failure
    EXPECT_EQ(DDBGet(ds.path().c_str(), "missing.txt", &out), DDBERR_NOTFOUND);
    EXPECT_EQ(DDBStac(ds.path().c_str(), "missing.txt", "", "", "", &out), DDBERR_EXCEPTION);
    EXPECT_EQ(out, kUntouched);

    // DDBBuild: BUILDDEPMISSING (1.x), with a message
    const auto shp = ds.root / "roads.shp";
    writeFile(shp, "not a real shapefile: its .shx and .dbf are missing");
    char* added = nullptr;
    const std::string shpPath = shp.string();
    const char* paths[] = {shpPath.c_str()};
    ASSERT_DDB_OK(DDBAdd(ds.path().c_str(), paths, 1, &added, false));
    take(added);
    EXPECT_EQ(DDBBuild(ds.path().c_str(), "roads.shp", nullptr, false, false), DDBERR_BUILDDEPMISSING);
    EXPECT_STRNE(DDBGetLastError(), "");

    // Invalid arguments of an export of 1.x: still EXCEPTION
    EXPECT_EQ(DDBGet(nullptr, "a.txt", &out), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBBuild(nullptr, nullptr, nullptr, false, false), DDBERR_EXCEPTION);
    DDBReleaseConnections(ds.path().c_str(), nullptr);
}

// A retryable failure of DDBBuild (1.x): DDBERR_EXCEPTION, with the reason
TEST(ErrorMapper, retryableReasonOfAFrozenExport) {
    const Dataset ds(TEST_NAME);
    testutils::GeoRasterSpec spec;
    spec.width = 64;
    spec.height = 64;
    const auto ortho = testutils::createGeoRaster(ds.root / "ortho.tif", spec);
    char* added = nullptr;
    const std::string orthoPath = ortho.string();
    const char* paths[] = {orthoPath.c_str()};
    ASSERT_DDB_OK(DDBAdd(ds.path().c_str(), paths, 1, &added, false));
    take(added);

    {
        // More free space than any disk has
        const EnvVarGuard minFree("DDB_MIN_FREE_DISK_MB", "999999999999");
        EXPECT_EQ(DDBBuild(ds.path().c_str(), "ortho.tif", nullptr, false, false), DDBERR_EXCEPTION);
        EXPECT_STRNE(DDBGetLastError(), "");
        EXPECT_STREQ(DDBGetLastErrorReason(), "lowDiskSpace");
    }

    // The next call clears the reason
    char* tag = nullptr;
    ASSERT_DDB_OK(DDBGetTag(ds.path().c_str(), &tag));
    take(tag);
    EXPECT_STREQ(DDBGetLastErrorReason(), "");
    DDBReleaseConnections(ds.path().c_str(), nullptr);
}

// ------------------------------------------------------------------ outputs are written last

TEST(Outputs, stringOutputIsWrittenOnSuccessOnly) {
    char* out = kUntouched;
    EXPECT_EQ(capi::guardedOutput(&out,
                                  [&]() -> std::string {
                                      EXPECT_EQ(out, kUntouched) << "written before the body returned";
                                      throw InvalidArgsException("late failure");
                                  }),
              DDBERR_EXCEPTION);
    EXPECT_EQ(out, kUntouched);

    EXPECT_EQ(capi::guardedOutput(&out, [] { return json{{"a", 1}}; }), DDBERR_NONE);
    EXPECT_EQ(take(out), R"({"a":1})");
}

TEST(Outputs, valueOutputIsWrittenOnSuccessOnly) {
    bool flag = false;
    EXPECT_EQ(capi::guardedOutput(&flag, []() -> bool { throw FSException("late failure"); }), DDBERR_EXCEPTION);
    EXPECT_FALSE(flag);
    EXPECT_EQ(capi::guardedOutput(&flag, [] { return true; }), DDBERR_NONE);
    EXPECT_TRUE(flag);

    // An optional output (NULL) is skipped
    EXPECT_EQ(capi::guardedOutput(static_cast<int*>(nullptr), [] { return 1; }), DDBERR_NONE);
}

// A buffer the library made before the failure is freed (LeakSanitizer) and never handed out
TEST(Outputs, bufferOfAFailedCallIsFreed) {
    uint8_t* buffer = reinterpret_cast<uint8_t*>(kUntouched);
    int size = -1;
    EXPECT_EQ(capi::guardedBuffer(&buffer, &size,
                                  [](uint8_t** b, int* s) {
                                      *b = static_cast<uint8_t*>(VSIMalloc(16));
                                      *s = 16;
                                      throw GDALException("encoder failed after the allocation");
                                  }),
              DDBERR_EXCEPTION);
    EXPECT_EQ(buffer, reinterpret_cast<uint8_t*>(kUntouched));
    EXPECT_EQ(size, -1);

    EXPECT_EQ(capi::guardedBuffer(&buffer, &size,
                                  [](uint8_t** b, int* s) {
                                      *b = static_cast<uint8_t*>(VSIMalloc(8));
                                      *s = 8;
                                  }),
              DDBERR_NONE);
    EXPECT_NE(buffer, reinterpret_cast<uint8_t*>(kUntouched));
    EXPECT_EQ(size, 8);
    DDBVSIFree(buffer);
}

TEST(Outputs, exportsLeaveOutputsOfAFailedCall) {
    TestArea ta(TEST_NAME, true);
    const std::string missing = ta.getPath("missing.tif").string();
    char* out = kUntouched;
    uint8_t* buffer = reinterpret_cast<uint8_t*>(kUntouched);
    int size = -1;

    EXPECT_EQ(DDBInit(ta.getPath("no/such/folder").string().c_str(), &out), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBGetRasterInfo(missing.c_str(), &out), DDBERR_EXCEPTION);
    EXPECT_EQ(out, kUntouched);
    EXPECT_EQ(DDBMemoryTile(missing.c_str(), 0, 0, 0, &buffer, &size, 256, false, false, ""), DDBERR_EXCEPTION);
    EXPECT_EQ(DDBGenerateMemoryThumbnail(missing.c_str(), 64, &buffer, &size), DDBERR_EXCEPTION);
    EXPECT_EQ(buffer, reinterpret_cast<uint8_t*>(kUntouched));
    EXPECT_EQ(size, -1);
}

// ------------------------------------------------------------------ process initialization

TEST(ProcessInit, registerProcessReturnsNone) {
    EXPECT_EQ(DDBRegisterProcess(false), DDBERR_NONE);
    EXPECT_STREQ(DDBGetLastError(), "");
}

// Runs in the child process of the death test: a failed initialization stays failed
[[noreturn]] void stickyInitChild() {
    const auto fail = [](const char* check) {
        std::fprintf(stderr, "check failed: %s; last error: %s\n", check, DDBGetLastError());
        std::exit(1);
    };
    if (capi::initializeProcess([](bool) { throw AppException("probe cannot initialize"); }, false) !=
        DDBERR_EXCEPTION)
        fail("initializeProcess returns EXCEPTION");
    const std::string message = DDBGetLastError();
    if (message.find("probe cannot initialize") == std::string::npos)
        fail("the message names the initialization failure");

    // Every later guarded call fails with it, without running
    char* out = kUntouched;
    if (DDBNormalizePath("a/b", nullptr, &out) != DDBERR_EXCEPTION || out != kUntouched)
        fail("a later call fails and leaves its output");
    if (DDBGetLastError() != message)
        fail("a later call reports the initialization failure");
    if (DDBRegisterProcess(false) != DDBERR_EXCEPTION || DDBGetLastError() != message)
        fail("DDBRegisterProcess does not retry");

    // The exempt accessors still work, and keep the message
    if (DDBGetVersion() == nullptr || DDBFree(nullptr) != DDBERR_NONE || DDBGetCapabilities() == nullptr)
        fail("exempt accessors work");
    if (DDBGetLastError() != message)
        fail("exempt accessors keep the message");

    std::fprintf(stderr, "sticky init ok\n");
    std::exit(0);
}

// The initialization is process-wide: the failure is injected in a child process
TEST(ProcessInit, failureIsSticky) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(stickyInitChild(), ::testing::ExitedWithCode(0), "sticky init ok");
}

// ------------------------------------------------------------------ UTF-8 paths

// Restores the global locale on scope exit
struct LocaleGuard {
    std::string saved;
    LocaleGuard() : saved(std::setlocale(LC_ALL, nullptr)) {}
    ~LocaleGuard() { std::setlocale(LC_ALL, saved.c_str()); }
};

// A JPEG of width x height pixels (GDAL), without EXIF
void writeJpeg(const fs::path& path, int width, int height) {
    GDALDatasetH mem = GDALCreate(GDALGetDriverByName("MEM"), "", width, height, 3, GDT_Byte, nullptr);
    ASSERT_NE(mem, nullptr);
    for (int b = 1; b <= 3; b++)
        ASSERT_EQ(GDALFillRaster(GDALGetRasterBand(mem, b), 40.0 * b, 0.0), CE_None);
    GDALDatasetH jpeg = GDALCreateCopy(GDALGetDriverByName("JPEG"), io::toUtf8(path).c_str(), mem, FALSE,
                                       nullptr, nullptr, nullptr);
    ASSERT_NE(jpeg, nullptr) << CPLGetLastErrorMsg();
    GDALClose(jpeg);
    GDALClose(mem);
}

const json* findPath(const json& entries, const std::string& path) {
    for (const auto& e : entries)
        if (e.contains("path") && e["path"] == path)
            return &e;
    return nullptr;
}

// A dataset under a non-ASCII folder, with non-ASCII file names and the "C" locale (a host,
// .NET or another library, may change the process locale after DDBRegisterProcess): every
// path goes through io::fromUtf8 / io::toUtf8 (mio.h, "Path encoding"), never the ANSI code
// page of Windows. Covers the index (init, add, list, get), GDAL (GeoTIFF, COG build, thumbnail),
// the metadata helper (EXIF write and read of a JPEG), the build lock and delta/apply.
TEST(Utf8Paths, datasetUnderANonAsciiFolderWithTheCLocale) {
    TestArea ta(TEST_NAME, true);
    const std::string folderName = "ddb_citt\xC3\xA0_\xC3\xBC";  // "ddb_città_ü"
    const std::string fileName = "\xC3\xA4.txt";                  // "ä.txt"
    const std::string rasterName = "ortho_\xC3\xA9.tif";          // "ortho_é.tif"
    const std::string imageName = "pano_\xC3\xB6.jpg";            // "pano_ö.jpg"
    const fs::path folder = ta.getFolder() / io::fromUtf8(folderName);
    fs::create_directories(folder);
    writeFile(folder / io::fromUtf8(fileName), "alpha");
    testutils::GeoRasterSpec spec;
    spec.width = spec.height = 64;
    spec.value = [](int band, int x, int y) { return 20.0 + band * 30.0 + x + y; };
    testutils::createGeoRaster(folder / io::fromUtf8(rasterName), spec);
    // Twice as wide as high, with a GPS position: a GeoPanorama only when the file is read through the metadata helper
    writeJpeg(folder / io::fromUtf8(imageName), 64, 16);
    const std::string folderUtf8 = io::toUtf8(folder);
    const std::string fileUtf8 = io::toUtf8(folder / io::fromUtf8(fileName));
    const std::string rasterUtf8 = io::toUtf8(folder / io::fromUtf8(rasterName));
    const std::string imageUtf8 = io::toUtf8(folder / io::fromUtf8(imageName));

    const LocaleGuard locale;
    ASSERT_NE(std::setlocale(LC_ALL, "C"), nullptr);

    ASSERT_TRUE(ExifEditor(imageUtf8).SetGPS(46.0667, 11.1167, 194.0).empty());

    char* out = nullptr;
    ASSERT_DDB_OK(DDBInit(folderUtf8.c_str(), &out));
    EXPECT_EQ(io::fromUtf8(take(out)), folder / ".ddb");

    const char* paths[] = {fileUtf8.c_str(), rasterUtf8.c_str(), imageUtf8.c_str()};
    ASSERT_DDB_OK(DDBAdd(folderUtf8.c_str(), paths, 3, &out, false));
    const json added = json::parse(take(out));
    ASSERT_EQ(added.size(), 3u) << added.dump();
    const json* text = findPath(added, fileName);
    const json* raster = findPath(added, rasterName);
    const json* image = findPath(added, imageName);
    ASSERT_TRUE(text != nullptr && raster != nullptr && image != nullptr) << added.dump();
    EXPECT_EQ((*text)["type"], static_cast<int>(EntryType::Generic));
    EXPECT_EQ((*raster)["type"], static_cast<int>(EntryType::GeoRaster)) << raster->dump();
    EXPECT_EQ((*image)["type"], static_cast<int>(EntryType::GeoPanorama)) << image->dump();
    const std::string rasterHash = (*raster)["hash"];

    const char* listPaths[] = {folderUtf8.c_str()};
    ASSERT_DDB_OK(DDBList(folderUtf8.c_str(), listPaths, 1, &out, "json", true, 0));
    const json listed = json::parse(take(out));
    ASSERT_EQ(listed.size(), 3u) << listed.dump();
    EXPECT_NE(findPath(listed, fileName), nullptr);
    EXPECT_NE(findPath(listed, rasterName), nullptr);
    EXPECT_NE(findPath(listed, imageName), nullptr);

    ASSERT_DDB_OK(DDBGet(folderUtf8.c_str(), fileName.c_str(), &out));
    EXPECT_EQ(json::parse(take(out))["path"], fileName);

    // The COG build of the GeoTIFF, in the build folder of the non-ASCII dataset
    ASSERT_DDB_OK(DDBBuild(folderUtf8.c_str(), rasterName.c_str(), nullptr, false, false));
    ASSERT_DDB_OK(DDBResolveBuildFile(folderUtf8.c_str(), rasterHash.c_str(), "cog/cog.tif", &out));
    const fs::path cog = io::fromUtf8(json::parse(take(out))["path"].get<std::string>());
    ASSERT_TRUE(fs::exists(cog)) << io::toUtf8(cog);
    EXPECT_TRUE(fs::equivalent(cog, folder / ".ddb" / "build" / rasterHash / "cog" / "cog.tif"));
    EXPECT_GT(fs::file_size(cog), 0u);

    // A thumbnail of the GeoTIFF, written to a non-ASCII file name
    const fs::path thumb = folder / io::fromUtf8("thumb_\xC3\xA7.webp");  // "thumb_ç.webp"
    ASSERT_DDB_OK(DDBGenerateThumbnail(rasterUtf8.c_str(), 32, io::toUtf8(thumb).c_str()));
    EXPECT_GT(fs::file_size(thumb), 0u);

    // A build lock in the non-ASCII folder is seen by the probe, and released
    const std::string lockedUtf8 = io::toUtf8(folder / io::fromUtf8("out_\xC3\xB1"));  // "out_ñ"
    fs::create_directories(io::fromUtf8(lockedUtf8));
    {
        const BuildLock lock(lockedUtf8, false);
        EXPECT_TRUE(fs::exists(io::fromUtf8(buildLockPath(lockedUtf8))));
        EXPECT_TRUE(BuildLock::isHeld(lockedUtf8));
    }
    EXPECT_FALSE(BuildLock::isHeld(lockedUtf8));

    // Delta and apply: a second dataset under the non-ASCII folder receives the files
    const fs::path target = folder / io::fromUtf8("copia_\xC3\xA8");  // "copia_è"
    fs::create_directories(target);
    const std::string targetUtf8 = io::toUtf8(target);
    ASSERT_DDB_OK(DDBInit(targetUtf8.c_str(), &out));
    take(out);
    ASSERT_DDB_OK(DDBGetStamp(folderUtf8.c_str(), &out));
    const std::string sourceStamp = take(out);
    ASSERT_DDB_OK(DDBGetStamp(targetUtf8.c_str(), &out));
    const std::string targetStamp = take(out);
    ASSERT_DDB_OK(DDBDelta(sourceStamp.c_str(), targetStamp.c_str(), &out, "json"));
    const std::string delta = take(out);
    EXPECT_EQ(json::parse(delta)["adds"].size(), 3u) << delta;
    ASSERT_DDB_OK(DDBApplyDelta(delta.c_str(), folderUtf8.c_str(), targetUtf8.c_str(), MergeStrategy::KeepTheirs,
                                "[]", &out));
    EXPECT_EQ(json::parse(take(out)), json::array());
    EXPECT_TRUE(fs::exists(target / io::fromUtf8(fileName)));
    EXPECT_TRUE(fs::exists(target / io::fromUtf8(rasterName)));
    ASSERT_DDB_OK(DDBGet(targetUtf8.c_str(), imageName.c_str(), &out));
    EXPECT_EQ(json::parse(take(out))["type"], static_cast<int>(EntryType::GeoPanorama));

    int closed = 0;
    ASSERT_DDB_OK(DDBReleaseConnections(folderUtf8.c_str(), &closed));
    ASSERT_DDB_OK(DDBReleaseConnections(targetUtf8.c_str(), &closed));
}

}  // namespace
