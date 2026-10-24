/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 (XI-07): cancel tokens through the C API (new in 2.0): DDBCreateCancelToken, DDBCancel,
// DDBFreeCancelToken, DDBSetThreadCancelToken.
//
//  - every export listed as "cancellable" in DDBGetCapabilities() returns DDBERR_CANCELED, with
//    a message, when the thread's token is canceled;
//  - the raster analyses stop in the middle of their work (a cancel from the log callback of
//    their first message), and DDBApplyDelta is atomic to a cancel: cancelable while it reads
//    and hashes the files, whole once it changes the dataset;
//  - the thread's token is for the next call only: the call resets it when it returns;
//  - the host may free its token at any time, also while a parallel section works for it;
//  - a canceled statement leaves its pooled connection usable for the next call;
//  - DDBExportRaster2 has one cancellation: its progress callback and the token stop the same
//    checkpoints, GDAL's included;
//  - DDBShutdown stops the work in flight the same way, with DDBERR_SHUTDOWN.
// Threads meet on latches; no test sleeps nor asserts on a duration.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "c_api_util.h"
#include "cancellation.h"
#include "databasepool.h"
#include "dbops.h"
#include "ddb.h"
#include "delta.h"
#include "delta_apply.h"
#include "exceptions.h"
#include "gtest/gtest.h"
#include "hash.h"
#include "json.h"
#include "parallel.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace {

using namespace ddb;
namespace fs = std::filesystem;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

// A token the test owns; freed at the end of the scope (DDBFreeCancelToken)
class HostToken {
   public:
    HostToken() { EXPECT_EQ(DDBCreateCancelToken(&token_), DDBERR_NONE) << DDBGetLastError(); }
    ~HostToken() { DDBFreeCancelToken(token_); }
    HostToken(const HostToken&) = delete;
    HostToken& operator=(const HostToken&) = delete;
    DDBCancelToken* get() const { return token_; }
    // The host frees its reference early (the destructor then frees nothing)
    void free() {
        DDBFreeCancelToken(token_);
        token_ = nullptr;
    }

   private:
    DDBCancelToken* token_ = nullptr;
};

class CancelToken : public ::testing::Test {
   protected:
    void SetUp() override { capi::resetShutdownForTests(); }
    void TearDown() override { capi::resetShutdownForTests(); }
};

TEST_F(CancelToken, invalidArguments) {
    EXPECT_EQ(DDBCreateCancelToken(nullptr), DDBERR_INVALIDARGS);
    EXPECT_STRNE(DDBGetLastError(), "");
    EXPECT_EQ(DDBCancel(nullptr), DDBERR_INVALIDARGS);
    EXPECT_STRNE(DDBGetLastError(), "");

    // DDBFreeCancelToken is exempt like DDBFree: NULL is fine and the last error stays
    const std::string error = DDBGetLastError();
    EXPECT_EQ(DDBFreeCancelToken(nullptr), DDBERR_NONE);
    EXPECT_EQ(DDBGetLastError(), error);

    // NULL clears the thread's token
    ASSERT_DDB_OK(DDBSetThreadCancelToken(nullptr));
    EXPECT_FALSE(cancel::hasThreadToken());
}

TEST_F(CancelToken, cancelIsIdempotentAndWorksWhileShuttingDown) {
    HostToken token;
    ASSERT_DDB_OK(DDBCancel(token.get()));
    ASSERT_DDB_OK(DDBCancel(token.get()));
    ASSERT_DDB_OK(DDBShutdown(0));
    // A host cancels its requests while it shuts down; setting a token is a call like others
    ASSERT_DDB_OK(DDBCancel(token.get()));
    EXPECT_EQ(DDBSetThreadCancelToken(token.get()), DDBERR_SHUTDOWN);
    DDBCancelToken* another = nullptr;
    EXPECT_EQ(DDBCreateCancelToken(&another), DDBERR_SHUTDOWN);
    EXPECT_EQ(another, nullptr);
}

// The token is for the next call: that call resets it when it returns
TEST_F(CancelToken, threadTokenLastsForTheNextCallOnly) {
    HostToken token;
    ASSERT_DDB_OK(DDBCancel(token.get()));
    ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
    EXPECT_TRUE(cancel::hasThreadToken());  // setting it is not "the call"

    char* out = nullptr;
    ASSERT_DDB_OK(DDBNormalizePath("a/b", nullptr, &out));  // no checkpoint in its work
    DDBFree(out);
    EXPECT_FALSE(cancel::hasThreadToken());

    // A token set on one thread is not another thread's
    ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
    bool otherHasToken = true;
    std::thread([&] { otherHasToken = cancel::hasThreadToken(); }).join();
    EXPECT_FALSE(otherHasToken);
    ASSERT_DDB_OK(DDBSetThreadCancelToken(nullptr));
}

// No export returns CANCELED without a token, even when its work fails
TEST_F(CancelToken, noCanceledWithoutAToken) {
    char* out = nullptr;
    EXPECT_EQ(DDBGetRasterInfo("/no/such/raster.tif", &out), DDBERR_EXCEPTION);
}

// The inputs of the cancellable exports: a dataset with a georaster, built nowhere yet
struct Inputs {
    TestArea area;
    fs::path raster;   // 3 bands, EPSG:3857, 512x512 at (1000, 1000), 1 m pixels
    fs::path raster5;  // the same with 5 bands (spectral indexes)
    fs::path dem;  // 1 band Float32, EPSG:4326, a conical mound, 80x80 at (10, 45), 0.0001 degrees
    fs::path source;   // where the files of the deltas wait
    fs::path dataset;  // holds raster.tif
    explicit Inputs(const std::string& name) : area(name, true) {
        testutils::GeoRasterSpec spec;
        raster = testutils::createGeoRaster(area.getPath("raster.tif"), spec);
        spec.bands = 5;
        raster5 = testutils::createGeoRaster(area.getPath("raster5.tif"), spec);
        testutils::GeoRasterSpec mound;
        mound.width = 80;
        mound.height = 80;
        mound.bands = 1;
        mound.type = GDT_Float32;
        mound.epsg = 4326;
        mound.originX = 10.0;
        mound.originY = 45.0;
        mound.pixelSize = 0.0001;
        mound.value = [](int, int x, int y) {
            return std::max(0.0, 40.0 - std::hypot(x - 40.0, y - 40.0));
        };
        dem = testutils::createGeoRaster(area.getPath("mound.tif"), mound);
        source = area.getFolder("source");
        dataset = area.getFolder("ds");
        char* out = nullptr;
        EXPECT_EQ(DDBInit(dataset.string().c_str(), &out), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
        const auto inDataset = dataset / "ortho.tif";
        fs::copy_file(raster, inDataset);
        const std::string path = inDataset.string();
        const char* paths[] = {path.c_str()};
        EXPECT_EQ(DDBAdd(dataset.string().c_str(), paths, 1, &out, false), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
    }
};

// A call of each cancellable export, by name
std::map<std::string, std::function<DDBErr()>> cancellableCalls(Inputs& in) {
    static const double bbox[4] = {1000, 488, 1512, 1000};
    const std::string raster = in.raster.string();
    const std::string raster5 = in.raster5.string();
    const std::string dataset = in.dataset.string();
    const std::string out = in.area.getPath("out.tif").string();
    const std::string thumb = in.area.getPath("thumb.webp").string();
    const auto withBuffer = [](const std::function<DDBErr(uint8_t**, int*)>& call) {
        uint8_t* buffer = nullptr;
        int size = 0;
        const DDBErr rc = call(&buffer, &size);
        DDBVSIFree(buffer);
        return rc;
    };
    const std::string dem = in.dem.string();
    const std::string sourceFolder = in.source.string();
    const std::string masked = in.area.getPath("masked.tif").string();
    const std::string polygon =
        R"({"type":"Polygon","coordinates":[[[10.002,44.996],[10.006,44.996],[10.006,44.998],[10.002,44.998],[10.002,44.996]]]})";
    const std::string line =
        R"({"type":"LineString","coordinates":[[10.001,44.999],[10.007,44.993]]})";
    const auto withString = [](const std::function<DDBErr(char**)>& call) {
        char* text = nullptr;
        const DDBErr rc = call(&text);
        DDBFree(text);
        return rc;
    };
    const std::string orthoHash = ddb::Hash::fileSHA256((in.dataset / "ortho.tif").string());
    // A delta that adds a new file (staged in the source folder), applied to the dataset
    const auto counter = std::make_shared<int>(0);
    const auto applyAddedFile = [&in, dataset, sourceFolder, counter, withString](bool ex) {
        const std::string name = "added-" + std::to_string(++*counter) + ".txt";
        const std::string content = "added " + name;
        std::ofstream(in.source / name) << content;
        ddb::Delta d;
        d.adds.push_back(ddb::AddAction(name, ddb::Hash::strSHA256(content)));
        const std::string delta = json(d).dump();
        return withString([&](char** text) {
            return ex ? DDBApplyDeltaEx(delta.c_str(),
                                        sourceFolder.c_str(),
                                        dataset.c_str(),
                                        1,
                                        "[]",
                                        nullptr,
                                        text)
                      : DDBApplyDelta(delta.c_str(),
                                      sourceFolder.c_str(),
                                      dataset.c_str(),
                                      1,
                                      "[]",
                                      text);
        });
    };
    return {
        {"DDBApplyDelta", [=] { return applyAddedFile(false); }},
        {"DDBApplyDeltaEx", [=] { return applyAddedFile(true); }},
        {"DDBBuild", [=] { return DDBBuild(dataset.c_str(), "ortho.tif", nullptr, true, false); }},
        {"DDBCalculateVolume",
         [=] {
             return withString([&](char** text) {
                 return DDBCalculateVolume(dem.c_str(),
                                           polygon.c_str(),
                                           "lowest_perimeter",
                                           0.0,
                                           text);
             });
         }},
        {"DDBComputeDeltaLocals",
         [=] {
             ddb::Delta d;
             d.adds.push_back(ddb::AddAction("copy.tif", orthoHash));
             const std::string delta = json(d).dump();
             return withString([&](char** text) {
                 return DDBComputeDeltaLocals(delta.c_str(), dataset.c_str(), "", text);
             });
         }},
        {"DDBDetectAllStockpiles",
         [=] {
             return withString([&](char** text) {
                 return DDBDetectAllStockpiles(dem.c_str(), 0.5f, 0.0, 10, text);
             });
         }},
        {"DDBDetectStockpile",
         [=] {
             return withString([&](char** text) {
                 return DDBDetectStockpile(dem.c_str(), 44.996, 10.004, 100.0, 0.5f, text);
             });
         }},
        {"DDBExportRaster",
         [=] {
             return DDBExportRaster(raster.c_str(),
                                    out.c_str(),
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
         }},
        {"DDBExportRaster2",
         [=] {
             return DDBExportRaster2(raster.c_str(), out.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                     nullptr, nullptr);
         }},
        {"DDBGenerateContours",
         [=] {
             return withString([&](char** text) {
                 return DDBGenerateContours(dem.c_str(), 5.0, 0, 0.0, NAN, NAN, 0.0, 1, text);
             });
         }},
        {"DDBGenerateMemoryThumbnail",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBGenerateMemoryThumbnail(raster.c_str(), 256, b, s);
             });
         }},
        {"DDBGenerateMemoryThumbnailEx",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBGenerateMemoryThumbnailEx(raster.c_str(), 256, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, b, s);
             });
         }},
        {"DDBGenerateThumbnail",
         [=] { return DDBGenerateThumbnail(raster.c_str(), 256, thumb.c_str()); }},
        {"DDBGetRasterAreaStats",
         [=] {
             return withString([&](char** text) {
                 return DDBGetRasterAreaStats(raster.c_str(), 0, 0, 100, 100, text);
             });
         }},
        {"DDBGetRasterMetadata",
         [=] {
             return withString([&](char** text) {
                 return DDBGetRasterMetadata(raster.c_str(), nullptr, nullptr, text);
             });
         }},
        {"DDBGetRasterPointValue",
         [=] {
             return withString(
                 [&](char** text) { return DDBGetRasterPointValue(raster.c_str(), 10, 10, text); });
         }},
        {"DDBGetRasterProfile",
         [=] {
             return withString([&](char** text) {
                 return DDBGetRasterProfile(dem.c_str(), line.c_str(), 20, text);
             });
         }},
        {"DDBGetRasterValueInfo",
         [=] {
             return withString(
                 [&](char** text) { return DDBGetRasterValueInfo(raster.c_str(), text); });
         }},
        {"DDBMaskBorders",
         [=] { return DDBMaskBorders(raster.c_str(), masked.c_str(), 15, false); }},
        {"DDBMemoryTile",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) { return DDBMemoryTile(raster.c_str(), 14, 8192, 8191, b, s, 256, false, true, nullptr); });
         }},
        {"DDBMemoryTileEx",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBMemoryTileEx(raster.c_str(), 14, 8192, 8191, 256, false, true, nullptr, nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, b, s);
             });
         }},
        {"DDBMemoryTileFmt",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBMemoryTileFmt(raster.c_str(), 14, 8192, 8191, b, s, 256, false, true, nullptr, "png");
             });
         }},
        {"DDBRenderRasterIndex",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBRenderRasterIndex(raster5.c_str(), "NDVI", bbox, "EPSG:3857", 64, 64, "png", b, s);
             });
         }},
        {"DDBRenderRasterRegion",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) { return DDBRenderRasterRegion(raster.c_str(), bbox, "EPSG:3857", 64, 64, "png", b, s); });
         }},
        {"DDBRenderRasterRegionEx",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBRenderRasterRegionEx(raster.c_str(), bbox, "EPSG:3857", nullptr, nullptr, 0, 64, 64, "png", b, s);
             });
         }},
        {"DDBTile",
         [=] {
             char* tilePath = nullptr;
             const DDBErr rc = DDBTile(raster.c_str(), 14, 8192, 8191, &tilePath, 256, false, true);
             DDBFree(tilePath);
             return rc;
         }},
    };
}

// The capabilities list exactly the exports that honour a token: each returns CANCELED when it
// is canceled, and works without it
TEST_F(CancelToken, everyCancellableExportStopsWithACanceledToken) {
    Inputs in(TEST_NAME);
    // DDBTile writes into the cache folder: the test area's
    const std::string cacheDir = in.area.getFolder("cache").string();
    ASSERT_DDB_OK(DDBConfigure(json{{"cacheDir", cacheDir}}.dump().c_str()));
    struct ResetCacheDir {
        ~ResetCacheDir() { DDBConfigure(R"({"cacheDir": null})"); }
    } resetCacheDir;
    const auto calls = cancellableCalls(in);
    const json caps = json::parse(DDBGetCapabilities());
    std::vector<std::string> listed = caps["cancellable"].get<std::vector<std::string>>();
    std::vector<std::string> tested;
    for (const auto& [name, call] : calls)
        tested.push_back(name);
    EXPECT_EQ(listed, tested) << "a cancellable export without a call in this test, or the reverse";

    for (const auto& [name, call] : calls) {
        SCOPED_TRACE(name);
        {
            HostToken token;
            ASSERT_DDB_OK(DDBCancel(token.get()));
            ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
            EXPECT_EQ(call(), DDBERR_CANCELED);
            const std::string message = DDBGetLastError();
            EXPECT_NE(message.find(cancel::kCanceledByToken), std::string::npos) << message;
            EXPECT_FALSE(cancel::hasThreadToken());
        }
        // The same call without a token works (the token was reset by the canceled call)
        EXPECT_EQ(call(), DDBERR_NONE) << DDBGetLastError();
    }
}

// What a log callback received from the canceled calls
struct LoggedRecords {
    std::mutex mutex;
    std::vector<std::pair<int, std::string>> records;  // level, "source: message"
};

void collectRecords(int level, const char* message, const char* source, uint64_t, void* userData) {
    auto* logged = static_cast<LoggedRecords*>(userData);
    const std::lock_guard lock(logged->mutex);
    logged->records.emplace_back(level, std::string(source) + ": " + message);
}

// A canceled operation is not an error: GDAL reports the work its progress callback stopped as
// errors ("User terminated CreateCopy()", the PNG driver's "libpng: No IDATs written into file"),
// and the log callback of a host (Registry logs errors as errors) got them at ERROR level on every
// aborted request. They reach it at DEBUG level, with their text.
TEST_F(CancelToken, canceledExportsLogNoErrors) {
    Inputs in(TEST_NAME);
    const std::string cacheDir = in.area.getFolder("cache").string();
    ASSERT_DDB_OK(DDBConfigure(json{{"cacheDir", cacheDir}}.dump().c_str()));
    LoggedRecords logged;
    ASSERT_DDB_OK(DDBSetLogCallback(&collectRecords, DDB_LOG_TRACE, &logged));
    struct Reset {
        ~Reset() {
            DDBSetLogCallback(nullptr, DDB_LOG_TRACE, nullptr);
            DDBConfigure(R"({"cacheDir": null})");
        }
    } reset;

    for (const auto& [name, call] : cancellableCalls(in)) {
        SCOPED_TRACE(name);
        HostToken token;
        ASSERT_DDB_OK(DDBCancel(token.get()));
        ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
        EXPECT_EQ(call(), DDBERR_CANCELED);
    }

    const std::lock_guard lock(logged.mutex);
    bool gdalReportedTheCancellation = false;
    for (const auto& [level, text] : logged.records) {
        EXPECT_LT(level, DDB_LOG_ERROR) << "a canceled call logged an error: " << text;
        if (text.rfind("gdal: ", 0) == 0 && text.find("User terminated") != std::string::npos) {
            gdalReportedTheCancellation = true;
            EXPECT_EQ(level, DDB_LOG_DEBUG) << text;
        }
    }
    EXPECT_TRUE(gdalReportedTheCancellation)
        << "no GDAL error was raised by the canceled calls: the test checks nothing";
}

// Free after Cancel while a parallel section works for the token: the workers keep their own
// references (ASan checks the lifetime); the loop stops and the call returns CANCELED
TEST_F(CancelToken, freeAfterCancelDuringForEachIndex) {
    auto* token = new HostToken();
    std::latch started(1);
    std::latch release(1);
    constexpr size_t kCount = 64;
    std::atomic<size_t> ran{0};
    DDBErr rc = DDBERR_NONE;
    std::string message;
    std::thread caller([&] {
        ASSERT_DDB_OK(DDBSetThreadCancelToken(token->get()));
        rc = capi::guarded([&] {
            // Every item waits for the release, so indexes are left to start after the cancel
            parallel::forEachIndex(kCount, 4, [&](size_t i) {
                ran++;
                if (i == 0)
                    started.count_down();
                release.wait();
            });
        });
        message = DDBGetLastError();
    });
    started.wait();
    ASSERT_DDB_OK(DDBCancel(token->get()));
    delete token;  // DDBFreeCancelToken while the section runs
    release.count_down();
    caller.join();

    EXPECT_EQ(rc, DDBERR_CANCELED);
    EXPECT_EQ(message, cancel::kCanceledByToken);
    EXPECT_LT(ran.load(), kCount);
}

// A statement interrupted by a canceled token leaves its pooled connection usable: the next
// call on it works
TEST_F(CancelToken, canceledQueryLeavesThePooledConnectionUsable) {
    Inputs in(TEST_NAME);
    const std::string dataset = in.dataset.string();
    char* tag = nullptr;
    ASSERT_DDB_OK(DDBGetTag(dataset.c_str(), &tag));  // the connection is pooled now
    DDBFree(tag);

    HostToken token;
    std::latch started(1);
    DDBErr rc = DDBERR_NONE;
    std::string message;
    std::thread caller([&] {
        ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
        rc = capi::guarded([&] {
            const auto db = ddb::open(dataset, false);
            started.count_down();
            // Minutes of work unless the progress handler interrupts it
            auto q = db->query(
                "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 2000000000) "
                "SELECT count(*) FROM c");
            q->fetch();
        });
        message = DDBGetLastError();
    });
    started.wait();
    ASSERT_DDB_OK(DDBCancel(token.get()));
    token.free();
    caller.join();
    EXPECT_EQ(rc, DDBERR_CANCELED);
    EXPECT_EQ(message, cancel::kCanceledByToken);

    // The connection went back to the pool and serves the next call
    const auto before = DatabasePool::instance().stats();
    ASSERT_DDB_OK(DDBGetTag(dataset.c_str(), &tag));
    DDBFree(tag);
    const auto after = DatabasePool::instance().stats();
    EXPECT_EQ(after.reused, before.reused + 1);
    EXPECT_EQ(after.opened, before.opened);
    DDBReleaseConnections(dataset.c_str(), nullptr);
}

// One cancellation in DDBExportRaster2: the progress callback cancels the token while GDAL's
// translation runs (the callback receives GDAL's progress), and the translation stops with
// the token's message; the partial output is deleted
struct CancelFromCallback {
    DDBCancelToken* token;
    int calls = 0;
};

int cancelTokenInCallback(double fraction, const char*, void* userData) {
    auto* state = static_cast<CancelFromCallback*>(userData);
    state->calls++;
    if (fraction > 0.0 && fraction < 1.0)
        DDBCancel(state->token);
    return 0;  // the callback itself never asks to stop
}

TEST_F(CancelToken, exportRaster2TokenStopsGdalThroughTheSameCheckpoints) {
    TestArea area(TEST_NAME, true);
    testutils::GeoRasterSpec spec;
    spec.width = 2048;
    spec.height = 2048;
    const auto input = testutils::createGeoRaster(area.getPath("in.tif"), spec);
    const auto output = area.getPath("out.tif");

    HostToken token;
    CancelFromCallback state{token.get()};
    ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
    EXPECT_EQ(DDBExportRaster2(input.string().c_str(), output.string().c_str(), nullptr, "1,2,3", nullptr, nullptr,
                               nullptr, nullptr, 0, &cancelTokenInCallback, &state),
              DDBERR_CANCELED);
    const std::string message = DDBGetLastError();
    EXPECT_NE(message.find(cancel::kCanceledByToken), std::string::npos) << message;
    EXPECT_GE(state.calls, 2) << "GDAL's progress did not reach the callback";
    EXPECT_FALSE(fs::exists(output)) << "a canceled export must not leave an output file";
}

// DDBShutdown stops the work in flight at its next checkpoint: the call returns SHUTDOWN
TEST_F(CancelToken, shutdownStopsTheWorkInFlight) {
    std::latch started(1);
    std::latch release(1);
    DDBErr rc = DDBERR_NONE;
    std::string message;
    std::thread caller([&] {
        rc = capi::guarded([&] {
            // Every item waits for the release, so indexes are left to start after the shutdown
            parallel::forEachIndex(64, 4, [&](size_t i) {
                if (i == 0)
                    started.count_down();
                release.wait();
            });
        });
        message = DDBGetLastError();
    });
    started.wait();
    DDBErr shutdownRc = DDBERR_EXCEPTION;
    std::thread stopper([&] { shutdownRc = capi::shutdown(-1, [] {}); });
    while (!cancel::shutdownRequested())
        std::this_thread::yield();
    release.count_down();
    caller.join();
    stopper.join();
    EXPECT_EQ(rc, DDBERR_SHUTDOWN);
    EXPECT_EQ(message, cancel::kShuttingDown);
    EXPECT_EQ(shutdownRc, DDBERR_NONE);
}


// ------------------------------------------------------------------ raster analyses

// Cancels the token of the call the first time libddb logs a message that starts with `trigger`,
// on the thread that logs it: a cancel in the middle of the work, at a point the test chooses,
// without a thread or a timing
struct CancelOnMessage {
    DDBCancelToken* token = nullptr;
    std::string trigger;
    bool fired = false;
    bool shutdownToo = false;  // also the shutdown (what DDBShutdown does first)
};

void cancelOnMessage(int, const char* message, const char*, uint64_t, void* userData) {
    auto* state = static_cast<CancelOnMessage*>(userData);
    if (state->fired || std::string(message).rfind(state->trigger, 0) != 0)
        return;
    state->fired = true;
    DDBCancel(state->token);
    if (state->shutdownToo)
        cancel::requestShutdown();
}

// Sets the callback and the token for the next call, and puts the callback back
class CancelDuring {
public:
    CancelDuring(std::string trigger, bool shutdownToo = false) {
        state_.token = host_.get();
        state_.trigger = std::move(trigger);
        state_.shutdownToo = shutdownToo;
        EXPECT_EQ(DDBSetLogCallback(&cancelOnMessage, DDB_LOG_DEBUG, &state_), DDBERR_NONE);
    }
    ~CancelDuring() { DDBSetLogCallback(nullptr, DDB_LOG_DEBUG, nullptr); }
    void arm() { ASSERT_DDB_OK(DDBSetThreadCancelToken(host_.get())); }
    bool fired() const { return state_.fired; }

private:
    HostToken host_;
    CancelOnMessage state_;
};

// The analyses that read a raster stop at their next checkpoint after a cancel in the middle:
// the GDAL read or the loop over the rows
TEST_F(CancelToken, rasterAnalysesStopWhenCanceledInTheMiddle) {
    Inputs in(TEST_NAME);
    const std::string dem = in.dem.string();
    const std::string polygon =
        R"({"type":"Polygon","coordinates":[[[10.002,44.996],[10.006,44.996],[10.006,44.998],[10.002,44.998],[10.002,44.996]]]})";
    const struct {
        const char* name;
        const char* trigger;
        std::function<DDBErr(char**)> call;
    } analyses[] = {
        {"DDBCalculateVolume",
         "calculateVolume method=",
         [&](char** out) {
             return DDBCalculateVolume(dem.c_str(), polygon.c_str(), "lowest_perimeter", 0.0, out);
         }},
        {"DDBDetectStockpile",
         "detectStockpile path=",
         [&](char** out) {
             return DDBDetectStockpile(dem.c_str(), 44.996, 10.004, 100.0, 0.5f, out);
         }},
        {"DDBDetectAllStockpiles",
         "detectAllStockpiles path=",
         [&](char** out) { return DDBDetectAllStockpiles(dem.c_str(), 0.5f, 0.0, 10, out); }},
    };
    for (const auto& analysis : analyses) {
        SCOPED_TRACE(analysis.name);
        char* out = nullptr;
        {
            CancelDuring during(analysis.trigger);
            during.arm();
            EXPECT_EQ(analysis.call(&out), DDBERR_CANCELED);
            EXPECT_TRUE(during.fired())
                << "the work never logged its first message: the test checks nothing";
            EXPECT_NE(std::string(DDBGetLastError()).find(cancel::kCanceledByToken),
                      std::string::npos)
                << DDBGetLastError();
            EXPECT_EQ(out, nullptr);
        }
        EXPECT_EQ(analysis.call(&out), DDBERR_NONE) << DDBGetLastError();
        DDBFree(out);
    }
}

// A canceled mask leaves no output file
TEST_F(CancelToken, canceledMaskBordersLeavesNoOutput) {
    Inputs in(TEST_NAME);
    const std::string output = in.area.getPath("masked-out.tif").string();
    HostToken token;
    ASSERT_DDB_OK(DDBCancel(token.get()));
    ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
    EXPECT_EQ(DDBMaskBorders(in.raster.string().c_str(), output.c_str(), 15, false),
              DDBERR_CANCELED);
    EXPECT_FALSE(fs::exists(output));
    EXPECT_FALSE(fs::exists(output + ".msk"));
    EXPECT_EQ(DDBMaskBorders(in.raster.string().c_str(), output.c_str(), 15, false), DDBERR_NONE)
        << DDBGetLastError();
}

// ------------------------------------------------------------------ DDBApplyDelta

// What DDBApplyDelta does when it is canceled (XI-07): it is whole or it did not start. It stops at
// once while it reads and hashes the added files (nothing changed), and from its first change on
// it applies the delta whole and returns DDBERR_NONE, whatever the token or the shutdown say.
struct ApplyCase {
    Inputs in;
    std::string deltaJson;
    std::vector<std::string> names;

    ApplyCase(const std::string& name, int files) : in(name) {
        ddb::Delta d;
        d.adds.push_back(ddb::AddAction("folder", ""));
        for (int i = 0; i < files; i++) {
            const std::string path = "folder/file-" + std::to_string(i) + ".txt";
            fs::create_directories((in.source / path).parent_path());
            const std::string content = "content " + std::to_string(i);
            std::ofstream(in.source / path) << content;
            d.adds.push_back(ddb::AddAction(path, ddb::Hash::strSHA256(content)));
            names.push_back(path);
        }
        deltaJson = json(d).dump();
    }

    DDBErr apply(bool ex = false) {
        char* conflicts = nullptr;
        const std::string source = in.source.string();
        const std::string dataset = in.dataset.string();
        const DDBErr rc = ex ? DDBApplyDeltaEx(deltaJson.c_str(),
                                               source.c_str(),
                                               dataset.c_str(),
                                               1,
                                               "[]",
                                               nullptr,
                                               &conflicts)
                             : DDBApplyDelta(deltaJson.c_str(),
                                             source.c_str(),
                                             dataset.c_str(),
                                             1,
                                             "[]",
                                             &conflicts);
        DDBFree(conflicts);
        return rc;
    }

    std::vector<std::string> entries() const {
        char* out = nullptr;
        EXPECT_EQ(DDBListEntries(in.dataset.string().c_str(), "", true, "", "", false, &out),
                  DDBERR_NONE);
        std::vector<std::string> paths;
        for (const auto& e : json::parse(out))
            paths.push_back(e["path"].get<std::string>());
        DDBFree(out);
        return paths;
    }

    // Every added file is in the dataset and in the index, and no longer in the source folder
    void expectApplied() const {
        const auto indexed = entries();
        for (const auto& path : names) {
            EXPECT_TRUE(fs::exists(in.dataset / path)) << path;
            EXPECT_FALSE(fs::exists(in.source / path)) << path;
            EXPECT_NE(std::find(indexed.begin(), indexed.end(), path), indexed.end()) << path;
        }
    }

    // Nothing was applied: the dataset, its index and the source folder are as they were
    void expectNotApplied(const std::vector<std::string>& entriesBefore) const {
        for (const auto& path : names) {
            EXPECT_FALSE(fs::exists(in.dataset / path)) << path;
            EXPECT_TRUE(fs::exists(in.source / path)) << path;
        }
        EXPECT_EQ(entries(), entriesBefore);
    }
};

TEST_F(CancelToken, applyDeltaCanceledBeforeItChangesAnythingLeavesNothingApplied) {
    for (const bool ex : {false, true}) {
        SCOPED_TRACE(ex ? "DDBApplyDeltaEx" : "DDBApplyDelta");
        ApplyCase c(TEST_NAME, 20);
        const auto before = c.entries();
        {
            HostToken token;
            ASSERT_DDB_OK(DDBCancel(token.get()));
            ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
            EXPECT_EQ(c.apply(ex), DDBERR_CANCELED);
            EXPECT_NE(std::string(DDBGetLastError()).find(cancel::kCanceledByToken),
                      std::string::npos);
        }
        c.expectNotApplied(before);

        // The same call applies it: a canceled call is not a failed one
        EXPECT_EQ(c.apply(ex), DDBERR_NONE) << DDBGetLastError();
        c.expectApplied();
    }
}

// The token is canceled (or the shutdown starts) at the first message of the part that changes the
// dataset: the delta is applied whole, and the call returns DDBERR_NONE. Without the shield the
// first statement of the apply would see the token and leave the files moved before it unindexed.
TEST_F(CancelToken, applyDeltaCanceledOnceItChangesTheDatasetIsAppliedWhole) {
    for (const bool shutdownToo : {false, true}) {
        SCOPED_TRACE(shutdownToo ? "the shutdown too" : "the token");
        ApplyCase c(TEST_NAME, 60);
        {
            CancelDuring during("Applying a delta", shutdownToo);
            during.arm();
            EXPECT_EQ(c.apply(), DDBERR_NONE) << DDBGetLastError();
            EXPECT_TRUE(during.fired()) << "the apply never logged: the test checks nothing";
        }
        c.expectApplied();
        EXPECT_EQ(c.entries().size(), c.names.size() + 2u)
            << "the files, their folder and ortho.tif";
        capi::resetShutdownForTests();
    }
}

// The hash of a big file is a checkpoint, so the reading that is the long part of an apply stops
TEST_F(CancelToken, hashingAFileStopsWhenCanceledOrShuttingDown) {
    TestArea area(TEST_NAME, true);
    const auto file = area.getPath("big.bin");
    {
        std::ofstream out(file, std::ios::binary);
        const std::string block(1024 * 1024, 'x');
        for (int i = 0; i < 4; i++)
            out << block;
    }
    HostToken token;
    ASSERT_DDB_OK(DDBSetThreadCancelToken(token.get()));
    EXPECT_FALSE(ddb::Hash::fileSHA256(file.string()).empty());
    ASSERT_DDB_OK(DDBCancel(token.get()));
    EXPECT_THROW(ddb::Hash::fileSHA256(file.string()), CanceledException);
    ASSERT_DDB_OK(DDBSetThreadCancelToken(nullptr));
    EXPECT_FALSE(ddb::Hash::fileSHA256(file.string()).empty()) << "no token: no cancel";

    cancel::requestShutdown();
    EXPECT_THROW(ddb::Hash::fileSHA256(file.string()), ShutdownException);
    // An uninterruptible scope sees neither the token nor the shutdown, then they apply again
    {
        const cancel::UninterruptibleScope shield;
        EXPECT_FALSE(cancel::isCanceled());
        EXPECT_FALSE(ddb::Hash::fileSHA256(file.string()).empty());
    }
    EXPECT_TRUE(cancel::isCanceled());
}

}  // namespace
