/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 (XI-07): cancel tokens through the C API (new in 2.0): DDBCreateCancelToken, DDBCancel,
// DDBFreeCancelToken, DDBSetThreadCancelToken.
//
//  - every export listed as "cancellable" in DDBGetCapabilities() returns DDBERR_CANCELED, with
//    a message, when the thread's token is canceled;
//  - the thread's token is for the next call only: the call resets it when it returns;
//  - the host may free its token at any time, also while a parallel section works for it;
//  - a canceled statement leaves its pooled connection usable for the next call;
//  - DDBExportRaster2 has one cancellation: its progress callback and the token stop the same
//    checkpoints, GDAL's included;
//  - DDBShutdown stops the work in flight the same way, with DDBERR_SHUTDOWN.
// Threads meet on latches; no test sleeps nor asserts on a duration.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <latch>
#include <map>
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
#include "exceptions.h"
#include "gtest/gtest.h"
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
    fs::path dataset;  // holds raster.tif
    explicit Inputs(const std::string& name) : area(name, true) {
        testutils::GeoRasterSpec spec;
        raster = testutils::createGeoRaster(area.getPath("raster.tif"), spec);
        spec.bands = 5;
        raster5 = testutils::createGeoRaster(area.getPath("raster5.tif"), spec);
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
    return {
        {"DDBBuild", [=] { return DDBBuild(dataset.c_str(), "ortho.tif", nullptr, true, false); }},
        {"DDBExportRaster",
         [=] { return DDBExportRaster(raster.c_str(), out.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr); }},
        {"DDBExportRaster2",
         [=] {
             return DDBExportRaster2(raster.c_str(), out.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                     nullptr, nullptr);
         }},
        {"DDBGenerateMemoryThumbnail",
         [=] { return withBuffer([&](uint8_t** b, int* s) { return DDBGenerateMemoryThumbnail(raster.c_str(), 256, b, s); }); }},
        {"DDBGenerateMemoryThumbnailEx",
         [=] {
             return withBuffer([&](uint8_t** b, int* s) {
                 return DDBGenerateMemoryThumbnailEx(raster.c_str(), 256, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, b, s);
             });
         }},
        {"DDBGenerateThumbnail", [=] { return DDBGenerateThumbnail(raster.c_str(), 256, thumb.c_str()); }},
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

}  // namespace
