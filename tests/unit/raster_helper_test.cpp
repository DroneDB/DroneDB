/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The host side of the raster helper (raster_helper.h), on a stand-in for gdal-util that answers
// and breaks the way the name of the file in a request says (tests/helpers/fake_gdal_helper.cpp):
// what the index and the thumbnails do with each way a helper can fail, that the host survives
// all of them, which program the pool runs, that no helper outlives the shutdown, the version
// rule, cancellation, one probe request per file, a thumbnail that is not an image never reaching
// its destination, and the one-shot builds against a stand-in for the process runner. Waits are
// bounded deadlines, never sleeps used for synchronization.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cancellation.h"
#include "cpl_conv.h"
#include "dbops.h"
#include "entry.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "process_runner.h"
#include "raster_helper.h"
#include "testarea.h"
#include "thumbs.h"
#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
    using namespace ddb;
    using namespace ddb::raster;
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    using Clock = std::chrono::steady_clock;

    constexpr seconds kPatience(30);

    fs::path fakePath()
    {
        return io::getExeFolderPath() / DDB_FAKE_GDAL_HELPER_NAME;
    }

    // A client of the fake helper with room for two helpers and short deadlines, so a helper that
    // hangs costs the test a second and a half
    GdalUtilHelperOptions fakeOptions()
    {
        GdalUtilHelperOptions options;
        options.locate = [] { return fakePath(); };
        options.limit = [] { return size_t{2}; };
        options.probeTimeout = milliseconds(1500);
        options.thumbnailTimeout = milliseconds(1500);
        options.conversionTimeout = milliseconds(30000);
        return options;
    }

    // Whether a process is running (not a zombie), by the id a helper had
    bool processAlive(unsigned long pid)
    {
#ifdef _WIN32
        HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (handle == nullptr)
            return false;
        const bool running = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
        CloseHandle(handle);
        return running;
#elif defined(__linux__)
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(stat, line))
            return false;
        // "<pid> (<name>) <state> ...": the name may hold spaces and parentheses
        const size_t close = line.rfind(')');
        return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z' &&
               line[close + 2] != 'X';
#else
        return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
    }

    // Waits until the process is gone (killed, then reaped by its parent)
    bool goneWithin(unsigned long pid, seconds limit)
    {
        const auto deadline = Clock::now() + limit;
        while (processAlive(pid))
        {
            if (Clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(milliseconds(10));
        }
        return true;
    }

    std::vector<std::string> linesOf(const fs::path &file)
    {
        std::vector<std::string> lines;
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);)
            lines.push_back(line);
        return lines;
    }

    // The requests the fake logged for files with this name ("<pid> <op> <name>")
    std::vector<std::string> requestsFor(const fs::path &log, const std::string &op, const std::string &name)
    {
        std::vector<std::string> found;
        for (const auto &line : linesOf(log))
            if (line.size() > op.size() + name.size() + 2 &&
                line.compare(line.size() - op.size() - name.size() - 1, op.size() + name.size() + 1, op + " " + name) == 0)
                found.push_back(line);
        return found;
    }

    std::string readFile(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A dataset folder of files the fake knows by name, a request log, and the client of the
    // process pointed at the fake
    class RasterHelperTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            area_ = std::make_unique<TestArea>(
                std::string("RasterHelper.") + ::testing::UnitTest::GetInstance()->current_test_info()->name(), true);
            root_ = area_->getFolder("ds");
            log_ = area_->getPath("requests.log");
            logGuard_ = std::make_unique<EnvVarGuard>("DDB_FAKE_GDAL_LOG", io::toUtf8(log_));
            helper_ = std::make_unique<GdalUtilHelper>(fakeOptions());
            scope_ = std::make_unique<ScopedRasterHelper>(*helper_);
        }

        void TearDown() override
        {
            scope_.reset();
            helper_.reset();
        }

        fs::path touch(const std::string &name)
        {
            const fs::path file = root_ / name;
            std::ofstream(file, std::ios::binary) << "content of " << name;
            return file;
        }

        Entry parse(const std::string &name)
        {
            Entry entry;
            parseEntry(touch(name), root_, entry, false);
            return entry;
        }

        static std::string warningOf(const Entry &entry)
        {
            return entry.properties.is_object() ? entry.properties.value("warning", "") : std::string();
        }

        std::unique_ptr<TestArea> area_;
        fs::path root_;
        fs::path log_;
        std::unique_ptr<EnvVarGuard> logGuard_;
        std::unique_ptr<GdalUtilHelper> helper_;
        std::unique_ptr<ScopedRasterHelper> scope_;
    };

    // ------------------------------------------------------------------ the probe, as the index uses it

    TEST_F(RasterHelperTest, aGeoRasterEntryIsMadeFromTheResponseOfTheHelper)
    {
        const Entry entry = parse("ok.tif");
        EXPECT_EQ(entry.type, EntryType::GeoRaster);
        EXPECT_EQ(entry.properties.at("width"), 10);
        EXPECT_EQ(entry.properties.at("height"), 20);
        EXPECT_EQ(entry.properties.at("bands").size(), 1u);
        EXPECT_EQ(entry.properties.at("geotransform").size(), 6u);
        EXPECT_NE(entry.properties.at("projection").get<std::string>().find("WGS 84"), std::string::npos);
        EXPECT_EQ(entry.polygon_geom.points.size(), 5u);
        EXPECT_EQ(entry.point_geom.points.size(), 1u);
        EXPECT_TRUE(warningOf(entry).empty());
    }

    TEST_F(RasterHelperTest, aRasterWithoutProjectionIsAnImage)
    {
        const Entry entry = parse("plain.tif");
        EXPECT_EQ(entry.type, EntryType::Image);
        EXPECT_TRUE(entry.polygon_geom.empty());
        EXPECT_TRUE(warningOf(entry).empty());
    }

    TEST_F(RasterHelperTest, aVectorEntryIsMadeFromTheResponseOfTheHelper)
    {
        const Entry entry = parse("ok.geojson");
        EXPECT_EQ(entry.type, EntryType::Vector);
        ASSERT_TRUE(entry.properties.contains("vector"));
        EXPECT_EQ(entry.properties.at("vector").at("driver"), "GeoJSON");
        EXPECT_EQ(entry.polygon_geom.points.size(), 5u);
        EXPECT_TRUE(warningOf(entry).empty());
    }

    TEST_F(RasterHelperTest, oneProbeRequestServesTheDetectionAndTheMetadataOfAFile)
    {
        // A GeoRaster is detected by the Image handler and parsed by the GeoRaster handler: both
        // need the probe
        parse("ok.tif");
        EXPECT_EQ(requestsFor(log_, "probe", "ok.tif").size(), 1u);

        // An image GDAL knows no projection of is probed by the detection only
        parse("plain.tif");
        EXPECT_EQ(requestsFor(log_, "probe", "plain.tif").size(), 1u);

        // A file the helper could not read: the detection asked, the entry kept the answer
        parse("crash.tif");
        EXPECT_EQ(requestsFor(log_, "probe", "crash.tif").size(), 1u);

        parse("ok.geojson");
        EXPECT_EQ(requestsFor(log_, "probe", "ok.geojson").size(), 1u);
    }

    TEST_F(RasterHelperTest, theContextAnswersFromItsProbeOnlyForTheSameFileAndKind)
    {
        const fs::path first = touch("ok.tif");
        const fs::path second = touch("plain.tif");
        FingerprintContext ctx;

        const ProbeParams params;
        const ProbeOutcome *answer = &probeFile(ctx, first, params);
        EXPECT_EQ(&probeFile(ctx, first, params), answer);
        EXPECT_EQ(requestsFor(log_, "probe", "ok.tif").size(), 1u);

        // Another file, or another kind of probe, asks again
        EXPECT_EQ(probeFile(ctx, second, params).result->type, "Raster");
        EXPECT_EQ(requestsFor(log_, "probe", "plain.tif").size(), 1u);
        ProbeParams vector;
        vector.kind = ProbeKind::Vector;
        EXPECT_EQ(probeFile(ctx, second, vector).result->type, "Vector");
        EXPECT_EQ(requestsFor(log_, "probe", "plain.tif").size(), 2u);
    }

    // ------------------------------------------------------------------ a helper that breaks

    // The helper dies, never answers, answers what is not JSON, answers a line over the limit, the
    // id of another request or a result the protocol does not allow; or its worker reports the
    // limit it hit: the host keeps going, the entry is an image with a warning, and nothing is an
    // error of the item
    TEST_F(RasterHelperTest, aHelperThatBreaksLeavesAnImageWithAWarning)
    {
        for (const char *mode : {"crash", "hang", "garbage", "huge", "wrongid", "badresult", "limit", "crashed", "timeout"})
        {
            SCOPED_TRACE(mode);
            Entry entry;
            ASSERT_NO_THROW(parseEntry(touch(std::string(mode) + ".tif"), root_, entry, false));
            EXPECT_EQ(entry.type, EntryType::Image);
            EXPECT_EQ(warningOf(entry).rfind("raster not read: ", 0), 0u) << warningOf(entry);
            EXPECT_GT(warningOf(entry).size(), std::string("raster not read: ").size());
        }
    }

    TEST_F(RasterHelperTest, aHelperThatBreaksLeavesAVectorWithAWarning)
    {
        for (const char *mode : {"crash", "hang", "garbage", "huge", "limit"})
        {
            SCOPED_TRACE(mode);
            Entry entry;
            ASSERT_NO_THROW(parseEntry(touch(std::string(mode) + ".geojson"), root_, entry, false));
            EXPECT_EQ(entry.type, EntryType::Vector);
            EXPECT_EQ(warningOf(entry).rfind("raster not read: ", 0), 0u) << warningOf(entry);
            EXPECT_TRUE(entry.polygon_geom.empty());
        }
    }

    TEST_F(RasterHelperTest, theAddResultHasNoErrorForAFileTheHelperCouldNotRead)
    {
        initIndex(root_.string());
        auto db = ddb::open(root_.string(), true);
        AddOptions options;
        options.stopOnError = false;

        std::vector<std::string> paths;
        for (const char *name : {"crash.tif", "hang.tif", "garbage.tif", "huge.tif", "ok.tif", "crash.geojson"})
            paths.push_back(touch(name).string());

        AddResult result;
        ASSERT_NO_THROW(addToIndexEx(db.get(), paths, options, result));
        EXPECT_TRUE(result.errors.empty());
        EXPECT_EQ(result.entries.size(), paths.size());

        auto query = db->query("SELECT COUNT(*) FROM entries");
        query->fetch();
        EXPECT_EQ(query->getInt(0), static_cast<int>(paths.size()));
    }

    TEST_F(RasterHelperTest, afterAFailureTheNextRequestStartsANewHelper)
    {
        parse("crash.tif");
        EXPECT_EQ(helper_->startedHelpers(), 1u);
        EXPECT_EQ(helper_->idleHelpers(), 0u);

        const Entry entry = parse("ok.tif");
        EXPECT_EQ(entry.type, EntryType::GeoRaster);
        EXPECT_EQ(helper_->startedHelpers(), 2u);
    }

    TEST_F(RasterHelperTest, aFileGdalCannotOpenIsTheTypeOfItsExtensionWithoutWarning)
    {
        for (const char *mode : {"unreadable", "policy"})
        {
            SCOPED_TRACE(mode);
            const Entry raster = parse(std::string(mode) + ".tif");
            EXPECT_EQ(raster.type, EntryType::Image);
            EXPECT_TRUE(warningOf(raster).empty());

            const Entry vector = parse(std::string(mode) + ".geojson");
            EXPECT_EQ(vector.type, EntryType::Vector);
            EXPECT_TRUE(warningOf(vector).empty());
            EXPECT_TRUE(vector.polygon_geom.empty());
        }
    }

    TEST_F(RasterHelperTest, aFailureOfTheHelperItselfIsAnItemErrorForARasterAndAWarningForAVector)
    {
        Entry raster;
        EXPECT_THROW(parseEntry(touch("internal.tif"), root_, raster, false), GDALException);

        const Entry vector = parse("internal.geojson");
        EXPECT_EQ(vector.type, EntryType::Vector);
        EXPECT_EQ(warningOf(vector).rfind("vector parse failed: ", 0), 0u) << warningOf(vector);
    }

    TEST_F(RasterHelperTest, aFullDeviceIsAnItemErrorOfTheFileSystem)
    {
        Entry raster;
        try
        {
            parseEntry(touch("nospace.tif"), root_, raster, false);
            FAIL() << "no exception";
        }
        catch (const FSException &e)
        {
            EXPECT_EQ(e.code(), std::make_error_code(std::errc::no_space_on_device));
        }
        Entry vector;
        EXPECT_THROW(parseEntry(touch("nospace.geojson"), root_, vector, false), FSException);

        initIndex(root_.string());
        auto db = ddb::open(root_.string(), true);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        addToIndexEx(db.get(), {(root_ / "nospace.tif").string()}, options, result);
        ASSERT_EQ(result.errors.size(), 1u);
        EXPECT_EQ(result.errors[0].code, "FS");
    }

    TEST_F(RasterHelperTest, theClientMapsEachFailureToItsException)
    {
        const auto codeOf = [&](const std::string &name) {
            try
            {
                helper_->probe(touch(name), ProbeParams());
            }
            catch (const RasterOpException &e)
            {
                return std::string("op:") + toString(e.code());
            }
            catch (const RasterNotReadException &e)
            {
                return std::string("notread:") + toString(e.code());
            }
            catch (const FSException &)
            {
                return std::string("fs");
            }
            return std::string("none");
        };
        EXPECT_EQ(codeOf("unreadable.tif"), "op:unreadable");
        EXPECT_EQ(codeOf("policy.tif"), "op:policy");
        EXPECT_EQ(codeOf("internal.tif"), "op:internal");
        EXPECT_EQ(codeOf("limit.tif"), "notread:limit");
        EXPECT_EQ(codeOf("crashed.tif"), "notread:crashed");
        EXPECT_EQ(codeOf("timeout.tif"), "notread:timeout");
        EXPECT_EQ(codeOf("nospace.tif"), "fs");
        // The host's own detection of a broken helper
        EXPECT_EQ(codeOf("crash.tif"), "notread:crashed");
        EXPECT_EQ(codeOf("hang.tif"), "notread:timeout");
        EXPECT_EQ(codeOf("garbage.tif"), "notread:crashed");
        EXPECT_EQ(codeOf("huge.tif"), "notread:crashed");
        EXPECT_EQ(codeOf("ok.tif"), "none");
    }

    TEST_F(RasterHelperTest, aHelperThatHangsIsKilledAtTheDeadline)
    {
        const auto begin = Clock::now();
        EXPECT_THROW(helper_->probe(touch("hang.tif"), ProbeParams()), RasterNotReadException);
        EXPECT_LT(Clock::now() - begin, seconds(20));
        // The hung helper is not in the pool any more
        EXPECT_EQ(helper_->idleHelpers(), 0u);
        EXPECT_EQ(helper_->busyHelpers(), 0u);
    }

    // ------------------------------------------------------------------ the program, the version, shutdown

    TEST_F(RasterHelperTest, theProgramIsStartedWithTheEnvironmentOfAWorker)
    {
        const fs::path envLog = area_->getPath("env.log");
        EnvVarGuard guard("DDB_FAKE_GDAL_ENV_LOG", io::toUtf8(envLog));
        parse("ok.tif");
        const auto lines = linesOf(envLog);
        ASSERT_EQ(lines.size(), 2u);
        EXPECT_NE(lines[0].find("MALLOC_ARENA_MAX=2"), std::string::npos) << lines[0];
        EXPECT_NE(lines[1].find("PROJ_NETWORK=OFF"), std::string::npos) << lines[1];
    }

    TEST_F(RasterHelperTest, theEnvironmentVariableNamesTheProgramThePoolRuns)
    {
        EnvVarGuard guard("DDB_GDAL_UTIL_PATH", io::toUtf8(fakePath()));
        const fs::path versionLog = area_->getPath("version.log");
        EnvVarGuard versionGuard("DDB_FAKE_GDAL_VERSION_LOG", io::toUtf8(versionLog));

        // The default locate: tools::findTool("gdal-util", "DDB_GDAL_UTIL_PATH")
        GdalUtilHelper byEnvironment;
        const ProbeResult result = byEnvironment.probe(touch("ok.tif"), ProbeParams());
        EXPECT_EQ(result.type, "GeoRaster");

        // Only the stand-in answers "ok.tif" with a canned result and logs it
        EXPECT_EQ(requestsFor(log_, "probe", "ok.tif").size(), 1u);
        EXPECT_EQ(linesOf(versionLog).size(), 1u);
        byEnvironment.shutdown();
    }

    TEST_F(RasterHelperTest, noHelperProcessIsLeftAfterTheHelpersAreClosed)
    {
        EnvVarGuard guard("DDB_GDAL_UTIL_PATH", io::toUtf8(fakePath()));
        scope_.reset(); // the helper of the process, not the one of the fixture

        const ProbeResult result = rasterHelper().probe(touch("ok.tif"), ProbeParams());
        EXPECT_EQ(result.type, "GeoRaster");

        GdalUtilHelper &instance = GdalUtilHelper::instance();
        const std::vector<unsigned long> pids = instance.idleHelperPids();
        ASSERT_FALSE(pids.empty());
        for (const auto pid : pids)
            EXPECT_TRUE(processAlive(pid));

        EXPECT_GE(closeRasterHelpers(), pids.size());
        for (const auto pid : pids)
            EXPECT_TRUE(goneWithin(pid, kPatience)) << "helper " << pid << " still runs";
        EXPECT_TRUE(instance.idleHelperPids().empty());
    }

    TEST_F(RasterHelperTest, aMissingProgramStopsTheBatch)
    {
        GdalUtilHelperOptions options = fakeOptions();
        options.locate = [] { return fs::path(); };
        GdalUtilHelper missing(options);
        ScopedRasterHelper scope(missing);

        EXPECT_THROW(missing.probe(touch("ok.tif"), ProbeParams()), HelperUnavailableException);

        initIndex(root_.string());
        auto db = ddb::open(root_.string(), true);
        AddOptions addOptions;
        addOptions.stopOnError = false;
        AddResult result;
        EXPECT_THROW(addToIndexEx(db.get(), {touch("ok.tif").string(), touch("ok.geojson").string()}, addOptions, result),
                     HelperUnavailableException);
    }

    TEST_F(RasterHelperTest, aProtocolOfAnotherMajorVersionStopsTheBatch)
    {
        EnvVarGuard guard("DDB_FAKE_GDAL_PROTOCOL", "2.0");
        try
        {
            helper_->probe(touch("ok.tif"), ProbeParams());
            FAIL() << "no exception";
        }
        catch (const HelperUnavailableException &e)
        {
            EXPECT_NE(std::string(e.what()).find("protocol 2.0"), std::string::npos) << e.what();
        }
        EXPECT_EQ(helper_->idleHelpers(), 0u);

        initIndex(root_.string());
        auto db = ddb::open(root_.string(), true);
        AddOptions options;
        options.stopOnError = false;
        AddResult result;
        EXPECT_THROW(addToIndexEx(db.get(), {touch("ok.tif").string()}, options, result), HelperUnavailableException);
    }

    // ------------------------------------------------------------------ cancellation

    TEST_F(RasterHelperTest, aCanceledRequestKillsTheWorkerAndThrowsCanceled)
    {
        const fs::path input = touch("hang.tif");
        auto token = cancel::TokenRef::adopt(cancel::Token::create());

        // Cancels once the helper has the request: the log line is written when it reads it
        std::thread canceler([&] {
            const auto deadline = Clock::now() + kPatience;
            while (requestsFor(log_, "normalize", "hang.tif").empty() && Clock::now() < deadline)
                std::this_thread::sleep_for(milliseconds(5));
            token.get()->cancel("canceled by the test");
        });

        const auto begin = Clock::now();
        bool canceled = false;
        {
            cancel::ScopedThreadToken scope(token);
            try
            {
                helper_->normalize(input, area_->getPath("out.tif"), json::object());
            }
            catch (const CanceledException &)
            {
                canceled = true;
            }
        }
        canceler.join();
        EXPECT_TRUE(canceled);
        // The cancellation is looked at every half second; the deadline of the request is 30 s
        EXPECT_LT(Clock::now() - begin, seconds(20));

        const auto requests = requestsFor(log_, "normalize", "hang.tif");
        ASSERT_EQ(requests.size(), 1u);
        const unsigned long pid = std::stoul(requests[0].substr(0, requests[0].find(' ')));
        EXPECT_TRUE(goneWithin(pid, seconds(5))) << "helper " << pid << " still runs";
        EXPECT_EQ(helper_->idleHelpers(), 0u);
        EXPECT_EQ(helper_->busyHelpers(), 0u);
    }

    TEST_F(RasterHelperTest, aCallerWhoseWorkIsCanceledSendsNothing)
    {
        const fs::path input = touch("ok.tif");
        auto token = cancel::TokenRef::adopt(cancel::Token::create());
        token.get()->cancel("canceled by the test");
        cancel::ScopedThreadToken scope(token);

        EXPECT_THROW(helper_->probe(input, ProbeParams()), CanceledException);
        EXPECT_THROW(helper_->thumbnail(input, area_->getPath("t.webp"), {64, std::nullopt}), CanceledException);
        EXPECT_THROW(renderThumbnailWebp(input, {64, std::nullopt}), CanceledException);
        EXPECT_EQ(helper_->requestsSent(), 0u);
        EXPECT_EQ(helper_->startedHelpers(), 0u);
    }

    // ------------------------------------------------------------------ thumbnails

    // The thumbnail of a file, written like the callers write it
    fs::path makeThumbnail(const fs::path &input, const fs::path &out)
    {
        return generateThumb(input, 64, out, true, nullptr, nullptr);
    }

    TEST_F(RasterHelperTest, aThumbnailTheHelperWroteIsPublishedAtomically)
    {
        const fs::path out = area_->getPath("thumbs/ok.webp");
        fs::create_directories(out.parent_path());
        makeThumbnail(touch("ok.jpg"), out);
        ASSERT_TRUE(fs::exists(out));
        EXPECT_TRUE(looksLikeThumbnail(readFile(out)));
        // Nothing else is left beside it
        EXPECT_EQ(std::distance(fs::directory_iterator(out.parent_path()), fs::directory_iterator()), 1);
    }

    TEST_F(RasterHelperTest, aThumbnailThatIsNoImageNeverReachesTheCache)
    {
        const fs::path folder = area_->getFolder("thumbs");
        for (const char *mode : {"garbagethumb", "hugethumb", "linkthumb"})
        {
            SCOPED_TRACE(mode);
            const fs::path out = folder / (std::string(mode) + ".webp");
            EXPECT_THROW(makeThumbnail(touch(std::string(mode) + ".jpg"), out), GDALException);
            EXPECT_FALSE(fs::exists(out));
        }
        EXPECT_TRUE(fs::is_empty(folder));
    }

    TEST_F(RasterHelperTest, aThumbnailThatIsNoImageNeverReachesAMemoryBuffer)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        EXPECT_THROW(generateThumb(touch("garbagethumb.jpg"), 64, "", true, &buffer, &size), GDALException);
        EXPECT_EQ(buffer, nullptr);
    }

    TEST_F(RasterHelperTest, aThumbnailInMemoryIsTheFileTheHelperWrote)
    {
        uint8_t *buffer = nullptr;
        int size = 0;
        generateThumb(touch("ok.jpg"), 64, "", true, &buffer, &size);
        ASSERT_NE(buffer, nullptr);
        EXPECT_TRUE(looksLikeThumbnail(std::string(reinterpret_cast<const char *>(buffer), static_cast<size_t>(size))));
        VSIFree(buffer);
    }

    TEST_F(RasterHelperTest, theThumbnailFailuresAreGdalExceptions)
    {
        for (const char *mode : {"unreadable", "policy", "internal", "limit", "crash", "hang", "garbage"})
        {
            SCOPED_TRACE(mode);
            EXPECT_THROW(renderThumbnailWebp(touch(std::string(mode) + ".jpg"), {64, std::nullopt}), GDALException);
        }
        EXPECT_THROW(renderThumbnailWebp(touch("nospace.jpg"), {64, std::nullopt}), FSException);
        EXPECT_THROW(renderThumbnailWebp(touch("ok.jpg"), {0, std::nullopt}), InvalidArgsException);
    }

    // The helper reads files and never the network: a URL is downloaded by the host first, and a
    // download that fails never reaches the helper
    TEST_F(RasterHelperTest, aRemoteInputIsDownloadedByTheHostBeforeTheHelperSeesIt)
    {
        EXPECT_THROW(renderThumbnailWebp("http://127.0.0.1:9/remote.tif", {64, std::nullopt}), NetException);
        EXPECT_EQ(helper_->requestsSent(), 0u);
        EXPECT_EQ(helper_->startedHelpers(), 0u);
    }

    TEST(RasterThumbnailCheck, aThumbnailIsAWebpContainerOfTheSizeItSays)
    {
        std::string webp = "RIFF";
        webp += std::string("\x04\x00\x00\x00", 4);
        webp += "WEBP";
        EXPECT_TRUE(looksLikeThumbnail(webp));

        EXPECT_FALSE(looksLikeThumbnail(""));
        EXPECT_FALSE(looksLikeThumbnail("RIFF"));
        EXPECT_FALSE(looksLikeThumbnail("not an image at all, but long enough"));

        std::string wrongSize = webp;
        wrongSize[4] = 9;
        EXPECT_FALSE(looksLikeThumbnail(wrongSize));

        std::string otherFormat = webp;
        otherFormat.replace(8, 4, "WAVE");
        EXPECT_FALSE(looksLikeThumbnail(otherFormat));

        std::string jpeg = "\xff\xd8\xff\xe0";
        jpeg.resize(100, 'x');
        EXPECT_FALSE(looksLikeThumbnail(jpeg));
    }

    // ------------------------------------------------------------------ the protocol additions the client relies on

    TEST(RasterProbeParams, aSidecarDecisionTravelsWithTheParams)
    {
        ProbeParams undecided;
        undecided.kind = ProbeKind::Vector;
        EXPECT_FALSE(probeParamsFromJson(toJson(undecided)).sidecarDecided);

        // Decided: no .prj counts
        ProbeParams none = undecided;
        none.sidecarDecided = true;
        const ProbeParams noneBack = probeParamsFromJson(toJson(none));
        EXPECT_TRUE(noneBack.sidecarDecided);
        EXPECT_TRUE(noneBack.sidecar.empty());

        // Decided: this one does
        ProbeParams one = undecided;
        one.sidecarDecided = true;
        one.sidecar = io::fromUtf8("/data/a b/site.prj");
        const ProbeParams oneBack = probeParamsFromJson(toJson(one));
        EXPECT_TRUE(oneBack.sidecarDecided);
        EXPECT_EQ(oneBack.sidecar, one.sidecar);
    }

    TEST(RasterProbeResult, aWarningKeepsTheDrawingPropertiesAndReachesTheEntry)
    {
        ProbeResult result;
        result.type = "Vector";
        result.properties["georeferenced"] = true;
        result.warning = "vector parse failed: Invalid CAD coordinate system EPSG:0";

        const ProbeResult back = probeResultFromJson(toJson(result));
        EXPECT_EQ(back.warning, result.warning);

        Entry entry;
        applyProbe(back, entry);
        EXPECT_EQ(entry.properties.at("georeferenced"), true);
        EXPECT_EQ(entry.properties.at("warning"), result.warning);
        EXPECT_TRUE(entry.polygon_geom.empty());

        // No warning: the entry has none
        ProbeResult whole;
        whole.type = "Raster";
        Entry plain;
        applyProbe(probeResultFromJson(toJson(whole)), plain);
        EXPECT_FALSE(plain.properties.contains("warning"));
    }

    TEST(RasterProbeResult, aWarningThatIsNoTextIsABrokenResponse)
    {
        json result = toJson(ProbeResult());
        result["type"] = "Raster";
        result["warning"] = 7;
        EXPECT_THROW(probeResultFromJson(result), ProtocolError);
    }

    // ------------------------------------------------------------------ one-shot builds

    // Stands in for the process runner: records what it is asked to run and answers as the test says
    class FakeRunner : public process::IProcessRunner
    {
      public:
        using Handler = std::function<process::Result(const std::vector<std::string> &, const process::Options &)>;

        process::Result run(const fs::path &binary, const std::vector<std::string> &args,
                            const process::Options &options) const override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            programs.push_back(binary);
            calls.push_back(args);
            if (!args.empty() && args[0] == "--version")
            {
                process::Result version;
                version.started = true;
                version.exited = true;
                version.exitCode = 0;
                version.outputTail = versionText;
                return version;
            }
            return handler(args, options);
        }

        mutable std::mutex mutex_;
        mutable std::vector<fs::path> programs;
        mutable std::vector<std::vector<std::string>> calls;
        std::string versionText = "gdal-util test protocol 1.0";
        Handler handler;
    };

    int64_t idOfRequest(const std::vector<std::string> &args)
    {
        return json::parse(args.at(1)).at("id").get<int64_t>();
    }

    process::Result exitedWith(int code)
    {
        process::Result result;
        result.started = true;
        result.exited = true;
        result.exitCode = code;
        return result;
    }

    GdalUtilHelper oneShotHelper(FakeRunner &runner)
    {
        GdalUtilHelperOptions options = fakeOptions();
        options.runner = &runner;
        return GdalUtilHelper(options);
    }

    TEST(RasterOneShot, aBuildRunsTheProgramWithTheRequestAndForwardsTheProgress)
    {
        FakeRunner runner;
        runner.handler = [](const std::vector<std::string> &args, const process::Options &options) {
            options.onOutputLine("a message of a library");
            options.onOutputLine(R"({"progress":0.25})");
            options.onOutputLine(R"({"progress":1})");
            options.onOutputLine(R"({"id":)" + std::to_string(idOfRequest(args)) + R"(,"ok":true,"result":{}})");
            return exitedWith(0);
        };
        GdalUtilHelper helper = oneShotHelper(runner);

        std::vector<double> progress;
        const fs::path input = fs::absolute("input.tif");
        const fs::path output = fs::absolute("out/raster.cog");
        helper.buildCog(input, output, json{{"blocksize", 512}}, [&](double fraction) { progress.push_back(fraction); });

        EXPECT_EQ(progress, (std::vector<double>{0.25, 1.0}));
        ASSERT_EQ(runner.calls.size(), 2u);
        EXPECT_EQ(runner.calls[0], std::vector<std::string>{"--version"});
        ASSERT_EQ(runner.calls[1].size(), 2u);
        EXPECT_EQ(runner.calls[1][0], "--request");
        const Request request = decodeRequest(runner.calls[1][1]);
        EXPECT_EQ(request.op, op::kCog);
        EXPECT_EQ(request.path, input.lexically_normal());
        EXPECT_EQ(request.out, output.lexically_normal());
        EXPECT_EQ(request.params.at("blocksize"), 512);
        EXPECT_EQ(runner.programs[1], fakePath());
    }

    TEST(RasterOneShot, theVectorBuildIsTheSameRequestWithItsOwnOperation)
    {
        FakeRunner runner;
        runner.handler = [](const std::vector<std::string> &args, const process::Options &options) {
            options.onOutputLine(R"({"id":)" + std::to_string(idOfRequest(args)) + R"(,"ok":true,"result":{}})");
            return exitedWith(0);
        };
        GdalUtilHelper helper = oneShotHelper(runner);
        helper.buildVector(fs::absolute("a.geojson"), fs::absolute("out"), json::object(), ProgressCallback());
        EXPECT_EQ(decodeRequest(runner.calls.at(1).at(1)).op, op::kVector);
    }

    TEST(RasterOneShot, aFailureResponseIsMappedLikeAnyOther)
    {
        const std::vector<std::pair<std::string, std::string>> cases = {
            {"limit", "notread"}, {"unreadable", "op"}, {"internal", "op"}, {"nospace", "fs"}};
        for (const auto &[code, expected] : cases)
        {
            SCOPED_TRACE(code);
            FakeRunner runner;
            runner.handler = [code = code](const std::vector<std::string> &args, const process::Options &options) {
                options.onOutputLine(R"({"id":)" + std::to_string(idOfRequest(args)) + R"(,"ok":false,"error":")" + code +
                                     R"(","message":"it failed"})");
                return exitedWith(1);
            };
            GdalUtilHelper helper = oneShotHelper(runner);
            const auto build = [&] { helper.buildCog(fs::absolute("a.tif"), fs::absolute("o.tif"), json::object(), ProgressCallback()); };
            if (expected == "notread")
                EXPECT_THROW(build(), RasterNotReadException);
            else if (expected == "fs")
                EXPECT_THROW(build(), FSException);
            else
                EXPECT_THROW(build(), RasterOpException);
        }
    }

    TEST(RasterOneShot, aProcessThatEndsWithoutAResponseCrashed)
    {
        FakeRunner runner;
        runner.handler = [](const std::vector<std::string> &, const process::Options &options) {
            options.onOutputLine("terminate called after throwing an instance");
            process::Result result = exitedWith(-1);
            result.exited = false;
            result.termSignal = 6;
            result.outputTail = "terminate called after throwing an instance";
            return result;
        };
        GdalUtilHelper helper = oneShotHelper(runner);
        try
        {
            helper.buildCog(fs::absolute("a.tif"), fs::absolute("o.tif"), json::object(), ProgressCallback());
            FAIL() << "no exception";
        }
        catch (const RasterNotReadException &e)
        {
            EXPECT_EQ(e.code(), ErrorCode::Crashed);
            EXPECT_NE(std::string(e.what()).find("terminate called"), std::string::npos) << e.what();
        }
    }

    TEST(RasterOneShot, aResponseForAnotherRequestIsNoResponse)
    {
        FakeRunner runner;
        runner.handler = [](const std::vector<std::string> &args, const process::Options &options) {
            options.onOutputLine(R"({"id":)" + std::to_string(idOfRequest(args) + 1000) + R"(,"ok":true,"result":{}})");
            return exitedWith(0);
        };
        GdalUtilHelper helper = oneShotHelper(runner);
        EXPECT_THROW(helper.buildCog(fs::absolute("a.tif"), fs::absolute("o.tif"), json::object(), ProgressCallback()),
                     RasterNotReadException);
    }

    TEST(RasterOneShot, aProcessThatPrintsNothingForTheIdleTimeIsARetryableFailure)
    {
        FakeRunner runner;
        runner.handler = [](const std::vector<std::string> &, const process::Options &) {
            process::Result result;
            result.started = true;
            result.timedOut = true;
            result.duration = std::chrono::minutes(60);
            return result;
        };
        GdalUtilHelper helper = oneShotHelper(runner);
        try
        {
            helper.buildCog(fs::absolute("a.tif"), fs::absolute("o.tif"), json::object(), ProgressCallback());
            FAIL() << "no exception";
        }
        catch (const RetryableBuildException &e)
        {
            EXPECT_EQ(e.reason(), RetryableBuildReason::IdleTimeout);
        }
    }

    TEST(RasterOneShot, aProgramOfAnotherProtocolMajorIsNotRun)
    {
        FakeRunner runner;
        runner.versionText = "gdal-util test protocol 2.0";
        runner.handler = [](const std::vector<std::string> &, const process::Options &) {
            ADD_FAILURE() << "the request must not be sent";
            return exitedWith(0);
        };
        GdalUtilHelper helper = oneShotHelper(runner);
        EXPECT_THROW(helper.buildCog(fs::absolute("a.tif"), fs::absolute("o.tif"), json::object(), ProgressCallback()),
                     HelperUnavailableException);
        EXPECT_EQ(runner.calls.size(), 1u);
    }

    TEST(RasterOneShot, aProgramThatCannotStartIsUnavailable)
    {
        FakeRunner runner;
        runner.handler = [](const std::vector<std::string> &, const process::Options &) {
            process::Result result;
            result.errorMessage = "fork(): Cannot allocate memory";
            return result;
        };
        GdalUtilHelper helper = oneShotHelper(runner);
        EXPECT_THROW(helper.buildVector(fs::absolute("a.geojson"), fs::absolute("o"), json::object(), ProgressCallback()),
                     HelperUnavailableException);
    }

} // namespace
