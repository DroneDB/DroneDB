/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The environment integer parser, the cached tool locator and the tool runners, driven
// through a fake process runner: arguments, options, exit-code / timeout / cancel mapping and a
// missing tool, without running any tool.

#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "bm3d_runner.h"
#include "build_budget.h"
#include "buildlod_runner.h"
#include "cad.h"
#include "env.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "obj2tiles_runner.h"
#include "parallel.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "tool_locator.h"
#include "tool_run_support.h"
#include "tool_runner_result.h"
#include "untwine_runner.h"
#include "utils.h"

namespace {

using namespace ddb;

// ------------------------------------------------------------------------ helpers

// Returns what the test scripted, and records what the runner was given
class FakeProcessRunner : public process::IProcessRunner {
public:
    struct Call {
        fs::path binary;
        std::vector<std::string> args;
        process::Options options;
    };

    process::Result result = succeeded();
    // Runs before the result is returned: writes the files the tool would produce, or throws
    std::function<void(const Call &)> effect;

    process::Result run(const fs::path &binary, const std::vector<std::string> &args,
                        const process::Options &options) const override {
        calls.push_back({binary, args, options});
        if (effect)
            effect(calls.back());
        return result;
    }

    static process::Result succeeded() {
        process::Result r;
        r.started = true;
        r.exited = true;
        r.exitCode = 0;
        return r;
    }

    static process::Result exited(int code, const std::string &output = "") {
        process::Result r = succeeded();
        r.exitCode = code;
        r.outputTail = output;
        return r;
    }

    static process::Result timedOutAfterIdle() {
        process::Result r;
        r.started = true;
        r.exited = false;
        r.timedOut = true;
        r.errorMessage = "killed after 3600 s without output";
        return r;
    }

    mutable std::vector<Call> calls;
};

// A file the locator takes for a tool: executable on POSIX, any regular file on Windows
fs::path fakeTool(TestArea &ta, const std::string &name) {
    const fs::path path = ta.getPath(name);
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/sh\n";
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
    return path;
}

void touch(const fs::path &path, const std::string &content = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

bool hasArgs(const std::vector<std::string> &args, const std::vector<std::string> &expected) {
    return args == expected;
}

bool contains(const std::vector<std::string> &args, const std::string &arg) {
    return std::find(args.begin(), args.end(), arg) != args.end();
}

std::optional<std::string> envOverride(const process::Options &options, const std::string &name) {
    for (const auto &[key, value] : options.envOverrides)
        if (key == name)
            return value;
    return std::nullopt;
}

// ------------------------------------------------------------------------ env::

TEST(EnvSettings, parseDecimalAcceptsOnlyDigitsInRange) {
    EXPECT_EQ(env::parseDecimal("12", 1, 100), 12u);
    EXPECT_EQ(env::parseDecimal("007", 1, 100), 7u);
    EXPECT_EQ(env::parseDecimal("100", 1, 100), 100u);
    EXPECT_EQ(env::parseDecimal("0", 0, 100), 0u);

    for (const char *bad : {"", "0", "101", "-1", "+1", " 1", "1 ", "12abc", "0x10", "1e3", "1.5", "abc",
                            "99999999999999999999999"})
        EXPECT_FALSE(env::parseDecimal(bad, 1, 100)) << bad;
    // the largest uint64 is not mistaken for an overflow, nor is one more accepted
    EXPECT_EQ(env::parseDecimal("18446744073709551615", 0, UINT64_MAX), UINT64_MAX);
    EXPECT_FALSE(env::parseDecimal("18446744073709551616", 0, UINT64_MAX));
}

TEST(EnvSettings, readIntegerTellsUnsetFromInvalid) {
    const char *name = "DDB_TEST_INT";
    EnvVarGuard guard(name);

    unsetEnv(name);
    EXPECT_EQ(env::readPositiveInt(name).state, env::Integer::State::Unset);

    setEnv(name, "8");
    auto v = env::readPositiveInt(name);
    EXPECT_EQ(v.state, env::Integer::State::Valid);
    EXPECT_EQ(v.value, 8u);

    for (const char *bad : {"0", "-3", "eight", "8 cores"}) {
        setEnv(name, bad);
        v = env::readPositiveInt(name);
        EXPECT_EQ(v.state, env::Integer::State::Invalid) << bad;
        EXPECT_EQ(v.raw, bad);
    }

    setEnv(name, "70000");
    EXPECT_EQ(env::readPositiveInt(name, 65535).state, env::Integer::State::Invalid);
    EXPECT_FALSE(env::readIntegerOrWarn(name, 1, 65535));
    EXPECT_EQ(env::readIntegerOrWarn(name, 1, 100000), 70000u);

    setEnv(name, "0");
    EXPECT_EQ(env::readIntegerOrWarn(name, 0, 10), 0u);
}

TEST(EnvSettings, readSizeIsTheSettingOfThePoolsAndCaches) {
    const char *name = "DDB_TEST_SIZE";
    EnvVarGuard guard(name);

    unsetEnv(name);
    EXPECT_EQ(env::readSize(name, 7), 7u);

    setEnv(name, "0");  // allowed: it turns a pool or a cache off
    EXPECT_EQ(env::readSize(name, 7), 0u);

    setEnv(name, "12");
    EXPECT_EQ(env::readSize(name, 7), 12u);

    // std::stoul read 12, a huge number, 0 and 1 from these
    for (const char *bad : {"12abc", "-1", "0x10", "1e3", "4294967296", ""}) {
        setEnv(name, bad);
        EXPECT_EQ(env::readSize(name, 7), 7u) << bad;
    }
}

// ------------------------------------------------------------------------ tools::findTool

TEST(ToolLocator, overrideIsAuthoritativeAndPartOfTheCacheKey) {
    TestArea ta("runner_locator_override", true);
    const fs::path tool = fakeTool(ta, "bin/ddbtest-fake-tool");
    const char *var = "DDB_TEST_FAKE_TOOL_PATH";
    EnvVarGuard guard(var);

    setEnv(var, io::toUtf8(tool));
    EXPECT_EQ(tools::findTool("ddbtest-fake-tool", var), tool);

    // A different value of the variable is a new question, not the cached answer
    setEnv(var, io::toUtf8(ta.getPath("missing/ddbtest-fake-tool")));
    EXPECT_TRUE(tools::findTool("ddbtest-fake-tool", var).empty());

    setEnv(var, io::toUtf8(tool));
    EXPECT_EQ(tools::findTool("ddbtest-fake-tool", var), tool);
}

TEST(ToolLocator, theAnswerIsCachedUntilARefreshIsAsked) {
    TestArea ta("runner_locator_cache", true);
    const fs::path tool = fakeTool(ta, "bin/ddbtest-cached-tool");
    const char *var = "DDB_TEST_CACHED_TOOL_PATH";
    EnvVarGuard guard(var, io::toUtf8(tool));

    EXPECT_EQ(tools::findTool("ddbtest-cached-tool", var), tool);

    fs::remove(tool);
    EXPECT_EQ(tools::findTool("ddbtest-cached-tool", var), tool) << "served from the cache";
    EXPECT_TRUE(tools::findTool("ddbtest-cached-tool", var, true).empty()) << "forceRefresh looks again";
    EXPECT_TRUE(tools::findTool("ddbtest-cached-tool", var).empty()) << "and the new answer is cached";
}

TEST(ToolLocator, theNamedToolIsFoundOnThePath) {
    TestArea ta("runner_locator_path", true);
    const fs::path tool = fakeTool(ta, "pathdir/ddbtest-path-tool");
    EnvVarGuard path("PATH");
#ifdef _WIN32
    const char sep = ';';
    const fs::path expected = tool.parent_path() / "ddbtest-path-tool.exe";
    touch(expected);
#else
    const char sep = ':';
    const fs::path expected = tool;
#endif
    setEnv("PATH", io::toUtf8(tool.parent_path()) + sep + io::getEnvUtf8("PATH").value_or(""));

    EXPECT_EQ(tools::findTool("ddbtest-path-tool", nullptr, true), expected);
    EXPECT_TRUE(tools::findTool("ddbtest-no-such-tool", nullptr, true).empty());
}

// A host that loads libddb (the Registry runs as dotnet or its own apphost) has the tools
// next to libddb, not next to its executable
TEST(ToolLocator, theFoldersOfTheExecutableAndOfLibddbAreSearchedInOrder) {
    const fs::path exe = fs::path("hostroot") / "app";
    const fs::path lib = fs::path("ddbroot") / "lib";
    EXPECT_EQ(tools::searchFolders(exe, lib),
              (std::vector<fs::path>{exe, fs::path("hostroot") / "bin", lib, fs::path("ddbroot") / "bin"}));
}

TEST(ToolLocator, aStaticallyLinkedProgramSearchesItsFolderOnce) {
    const fs::path exe = fs::path("root") / "bin";
    EXPECT_EQ(tools::searchFolders(exe, exe), (std::vector<fs::path>{exe}));
    EXPECT_EQ(tools::searchFolders(exe, {}), (std::vector<fs::path>{exe}));
    EXPECT_TRUE(tools::searchFolders({}, {}).empty());
}

TEST(ToolLocator, theLibraryFolderIsTheFolderOfTheFileThatHoldsDroneDB) {
    // ddbtest links DroneDB statically: the code is in the executable itself
    const fs::path libDir = io::getLibraryFolderPath();
    ASSERT_FALSE(libDir.empty());
    EXPECT_TRUE(fs::equivalent(libDir, io::getExeFolderPath())) << io::toUtf8(libDir);
}

TEST(ToolLocator, theRunnersFindTheirToolThroughIt) {
    TestArea ta("runner_locator_runners", true);
    const fs::path tool = fakeTool(ta, "bin/tool");
    for (const auto& [var, find] : std::vector<std::pair<const char*, std::function<fs::path()>>>{
             {"DDB_OBJ2TILES_PATH", [] { return obj2tiles::findObj2TilesBinary(true); }},
             {"DDB_UNTWINE_PATH", [] { return untwine::findUntwineBinary(true); }},
             {"DDB_BUILDLOD_PATH", [] { return buildlod::findBuildLodBinary(true); }},
             {"DDB_DWG2DXF_PATH", [] { return cad::findDwg2DxfBinary(true); }},
             {"DDB_BM3D_PATH", [] { return bm3d::findBm3dBinary(true); }}}) {
        EnvVarGuard guard(var, io::toUtf8(tool));
        EXPECT_EQ(find(), tool) << var;
        setEnv(var, io::toUtf8(ta.getPath("missing")));
        EXPECT_TRUE(find().empty()) << var;
        guard.restore();
        find();  // leave the cache with the real answer
    }
}

// ------------------------------------------------------------------------ ToolRunResult

TEST(ToolResult, fromProcessMapsSuccessExitCodeAndTimeout) {
    const auto ok = ToolRunResult::fromProcess("tool", FakeProcessRunner::succeeded(), 100);
    EXPECT_TRUE(ok.succeeded);
    EXPECT_TRUE(ok.error.empty());

    const auto failed = ToolRunResult::fromProcess("tool", FakeProcessRunner::exited(3, "boom"), 100);
    EXPECT_FALSE(failed.succeeded);
    EXPECT_FALSE(failed.timedOut);
    EXPECT_EQ(failed.error, "tool exited with code 3: boom");

    const auto silent = ToolRunResult::fromProcess("tool", FakeProcessRunner::exited(1), 100);
    EXPECT_EQ(silent.error, "tool exited with code 1");

    const auto longOutput = ToolRunResult::fromProcess("tool", FakeProcessRunner::exited(2, "0123456789"), 4);
    EXPECT_EQ(longOutput.error, "tool exited with code 2: ...6789");

    const auto timedOut = ToolRunResult::fromProcess("tool", FakeProcessRunner::timedOutAfterIdle(), 100);
    EXPECT_FALSE(timedOut.succeeded);
    EXPECT_TRUE(timedOut.timedOut);
    EXPECT_THROW(timedOut.throwIfRetryable("ctx: "), RetryableBuildException);
    EXPECT_NO_THROW(failed.throwIfRetryable("ctx: "));
}

// ------------------------------------------------------------------------ Obj2Tiles

struct Obj2TilesFixture {
    TestArea ta;
    fs::path tool, input, outDir;
    obj2tiles::Obj2TilesOptions opts;
    FakeProcessRunner fake;
    std::string error;

    explicit Obj2TilesFixture(const std::string &name) : ta(name, true) {
        tool = fakeTool(ta, "bin/Obj2Tiles");
        input = ta.getPath("model.obj");
        touch(input);
        outDir = ta.getPath("out");
        // The tool writes tileset.json into its output folder
        fake.effect = [this](const FakeProcessRunner::Call &call) { touch(fs::path(call.args[1]) / "tileset.json", "{}"); };
    }

    bool run() { return obj2tiles::runObj2Tiles(input, outDir, opts, error, tool, fake); }
};

TEST(Obj2TilesRunner, buildsTheArgumentsAndOptions) {
    EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES");
    unsetEnv("DDB_OBJ2TILES_MAX_CORES");
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");

    Obj2TilesFixture f("runner_obj2tiles_args");
    ASSERT_TRUE(f.run()) << f.error;
    ASSERT_EQ(f.fake.calls.size(), 1u);
    const auto &call = f.fake.calls[0];
    EXPECT_EQ(call.binary, f.tool);
    EXPECT_TRUE(hasArgs(call.args, {io::toUtf8(f.input), io::toUtf8(f.outDir), "--divisions", "2", "--lods", "3",
                                    "--local"}));
    EXPECT_TRUE(static_cast<bool>(call.options.onOutputLine));

    // Georeferenced, with the optional flags: a period decimal separator whatever the locale
    f.opts.localMode = false;
    f.opts.lat = 45.5;
    f.opts.lon = 9.25;
    f.opts.alt = 120.0;
    f.opts.octree = true;
    f.opts.lodTextureScale = 0.25;
    f.opts.textureFormat = "Ktx2";
    f.opts.ktx2Quality = 100;
    f.opts.splitStrategy = "AbsoluteCenter";
    fs::remove_all(f.outDir);
    ASSERT_TRUE(f.run()) << f.error;
    const auto &second = f.fake.calls[1];
    EXPECT_TRUE(hasArgs(second.args, {io::toUtf8(f.input), io::toUtf8(f.outDir), "--divisions", "2", "--lods", "3",
                                      "--lat", "45.5", "--lon", "9.25", "--alt", "120", "--octree",
                                      "--lod-texture-scale", "0.25", "--texture-format", "Ktx2", "--ktx2-quality",
                                      "100", "--split-strategy", "AbsoluteCenter"}));
}

TEST(Obj2TilesRunner, aCoreCapGoesToTheChildEnvironmentAndDropsKtx2Threads) {
    EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES", "1");
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");

    Obj2TilesFixture f("runner_obj2tiles_cap");
    f.opts.textureFormat = "Ktx2";
    f.opts.ktx2Threads = 4;
    ASSERT_TRUE(f.run()) << f.error;
    const auto &call = f.fake.calls.at(0);
    EXPECT_EQ(envOverride(call.options, "DOTNET_PROCESSOR_COUNT"), "1");
    EXPECT_FALSE(contains(call.args, "--ktx2-threads"));
    EXPECT_TRUE(contains(call.args, "--ktx2-quality"));
}

TEST(Obj2TilesRunner, withoutACapKtx2ThreadsReachTheTool) {
    EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES");
    unsetEnv("DDB_OBJ2TILES_MAX_CORES");
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");

    Obj2TilesFixture f("runner_obj2tiles_threads");
    f.opts.textureFormat = "Ktx2";
    f.opts.ktx2Threads = 3;
    // The pool may give fewer cores than the host has (other tests hold tokens): only check
    // the flag when no cap was set
    ASSERT_TRUE(f.run()) << f.error;
    const auto &call = f.fake.calls.at(0);
    if (!envOverride(call.options, "DOTNET_PROCESSOR_COUNT")) {
        EXPECT_TRUE(contains(call.args, "--ktx2-threads"));
    }
}

// The cap of a build (BuildOptions::threads) below the cores of the host sets the cores of the
// child like DDB_OBJ2TILES_MAX_CORES does, and the lower of the two counts
TEST(Obj2TilesRunner, theCapOfABuildGoesToTheChildEnvironmentAndDropsKtx2Threads) {
    if (parallel::hardwareThreads() < 2)
        GTEST_SKIP() << "a cap below the cores of the host needs two cores";
    EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES");
    unsetEnv("DDB_OBJ2TILES_MAX_CORES");
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    const int below = static_cast<int>(parallel::hardwareThreads()) - 1;

    Obj2TilesFixture f("runner_obj2tiles_build_cap");
    f.opts.maxCores = below;
    f.opts.textureFormat = "Ktx2";
    f.opts.ktx2Threads = 4;
    ASSERT_TRUE(f.run()) << f.error;
    const auto &call = f.fake.calls.at(0);
    EXPECT_EQ(envOverride(call.options, "DOTNET_PROCESSOR_COUNT"), std::to_string(below));
    EXPECT_FALSE(contains(call.args, "--ktx2-threads"));
}

// A cap of a build at or above the cores of the host limits nothing: the run is that of no cap,
// without DOTNET_PROCESSOR_COUNT and with the threads of the texture encoder
TEST(Obj2TilesRunner, aCapOfABuildThatDoesNotLowerTheCoresIsNoCap) {
    EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES");
    unsetEnv("DDB_OBJ2TILES_MAX_CORES");
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");
    // Every core of the host is free, so the pool lowers nothing either
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    const auto hw = static_cast<int>(parallel::hardwareThreads());

    for (const int cap : {hw, hw + 1, 4096}) {
        Obj2TilesFixture f("runner_obj2tiles_build_cap_none_" + std::to_string(cap));
        f.opts.maxCores = cap;
        f.opts.textureFormat = "Ktx2";
        f.opts.ktx2Threads = 4;
        ASSERT_TRUE(f.run()) << f.error;
        const auto &call = f.fake.calls.at(0);
        EXPECT_FALSE(envOverride(call.options, "DOTNET_PROCESSOR_COUNT")) << cap;
        const auto at = std::find(call.args.begin(), call.args.end(), "--ktx2-threads");
        ASSERT_NE(at, call.args.end()) << cap;
        ASSERT_NE(at + 1, call.args.end());
        EXPECT_EQ(*(at + 1), "4") << cap;
    }

    // Nor does a cap at or above an inherited DOTNET_PROCESSOR_COUNT replace it
    EnvVarGuard inherited("DOTNET_PROCESSOR_COUNT", "1");
    Obj2TilesFixture f("runner_obj2tiles_build_cap_inherited");
    f.opts.maxCores = 1;
    ASSERT_TRUE(f.run()) << f.error;
    EXPECT_FALSE(envOverride(f.fake.calls.at(0).options, "DOTNET_PROCESSOR_COUNT"));
}

TEST(Obj2TilesRunner, theLowerOfTheCapOfABuildAndTheVariableCounts) {
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 4096})");
    const auto atMost = [](size_t n) { return std::to_string(std::min(n, parallel::hardwareThreads())); };

    {
        EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES", "1");
        Obj2TilesFixture f("runner_obj2tiles_variable_lower");
        f.opts.maxCores = 3;
        ASSERT_TRUE(f.run()) << f.error;
        EXPECT_EQ(envOverride(f.fake.calls.at(0).options, "DOTNET_PROCESSOR_COUNT"), "1");
    }
    {
        EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES", "4096");
        Obj2TilesFixture f("runner_obj2tiles_cap_lower");
        f.opts.maxCores = 1;
        ASSERT_TRUE(f.run()) << f.error;
        EXPECT_EQ(envOverride(f.fake.calls.at(0).options, "DOTNET_PROCESSOR_COUNT"), atMost(1));
    }
}

TEST(Obj2TilesRunner, anInvalidCapIsIgnored) {
    EnvVarGuard cores("DDB_OBJ2TILES_MAX_CORES", "many");
    EnvVarGuard dotnet("DOTNET_PROCESSOR_COUNT");
    unsetEnv("DOTNET_PROCESSOR_COUNT");

    Obj2TilesFixture f("runner_obj2tiles_badcap");
    ASSERT_TRUE(f.run()) << f.error;
}

TEST(Obj2TilesRunner, anExitCodeIsAFailureWithTheOutputTail) {
    Obj2TilesFixture f("runner_obj2tiles_exit");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::exited(3, "Unhandled exception: bad mesh");
    EXPECT_FALSE(f.run());
    EXPECT_EQ(f.error, "Obj2Tiles exited with code 3: Unhandled exception: bad mesh");
}

TEST(Obj2TilesRunner, aTimeoutIsAFailure) {
    Obj2TilesFixture f("runner_obj2tiles_timeout");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::timedOutAfterIdle();
    EXPECT_FALSE(f.run());
    EXPECT_NE(f.error.find("Obj2Tiles "), std::string::npos) << f.error;
    EXPECT_NE(f.error.find("killed"), std::string::npos) << f.error;
}

TEST(Obj2TilesRunner, successWithoutATilesetIsAFailure) {
    Obj2TilesFixture f("runner_obj2tiles_notileset");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::exited(0, "done");
    EXPECT_FALSE(f.run());
    EXPECT_NE(f.error.find("tileset.json is missing or empty"), std::string::npos) << f.error;
}

TEST(Obj2TilesRunner, aMissingToolOrInputNeverStartsAChild) {
    Obj2TilesFixture f("runner_obj2tiles_missing");
    f.tool = f.ta.getPath("missing/Obj2Tiles");
    EXPECT_FALSE(f.run());
    EXPECT_EQ(f.error, "Obj2Tiles binary not found");

    f.tool = fakeTool(f.ta, "bin/Obj2Tiles");
    fs::remove(f.input);
    EXPECT_FALSE(f.run());
    EXPECT_NE(f.error.find("input OBJ does not exist"), std::string::npos) << f.error;

    // a destination that has files is refused before any launch
    touch(f.input);
    touch(f.outDir / "old.txt");
    EXPECT_FALSE(f.run());
    EXPECT_NE(f.error.find("must be absent or empty"), std::string::npos) << f.error;
    EXPECT_TRUE(f.fake.calls.empty());
}

TEST(Obj2TilesRunner, aCanceledChildPropagatesAndReleasesItsCpuTokens) {
    Obj2TilesFixture f("runner_obj2tiles_cancel");
    f.fake.effect = [](const FakeProcessRunner::Call &) { throw CanceledException("canceled"); };
    const auto before = parallel::cpuTokensInUse();
    EXPECT_THROW(f.run(), CanceledException);
    EXPECT_EQ(parallel::cpuTokensInUse(), before);
}

// ------------------------------------------------------------------------ bm3d

struct Bm3dFixture {
    TestArea ta;
    fs::path tool;
    fs::path input;
    fs::path output;
    FakeProcessRunner fake;

    explicit Bm3dFixture(const std::string& name)
        : ta(name, true),
          tool(fakeTool(ta, "bin/bm3d")),
          input(ta.getPath("model.obj")),
          output(ta.getPath("out/model.bm3d")) {
        touch(input);
        // the tool writes its output unless a test replaces this
        fake.effect = [this](const FakeProcessRunner::Call&) { touch(output, "bm3d"); };
    }

    void run() { bm3d::runBm3dBuild(input, output, tool, fake); }

    std::string failureMessage() {
        try {
            run();
        } catch (const AppException &e) {
            return e.what();
        }
        ADD_FAILURE() << "the build should have failed";
        return {};
    }
};

TEST(Bm3dRunner, runsTheToolOnTheInputAndTheOutput) {
    Bm3dFixture f("runner_bm3d");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 16})");
    EnvVarGuard threads("DDB_BUILD_THREADS", "1");
    f.run();
    ASSERT_EQ(f.fake.calls.size(), 1u);
    EXPECT_EQ(f.fake.calls[0].binary, f.tool);
    EXPECT_TRUE(hasArgs(f.fake.calls[0].args, {"--threads", "1", io::toUtf8(f.input), io::toUtf8(f.output)}));
    EXPECT_TRUE(fs::exists(f.output));
}

// The tool keeps busy the threads one build may use (DDB_BUILD_THREADS), as many as the CPU pool
// has free, and their tokens are held while it runs: a bm3d build counts against the same
// budget as the raster builds and the other tools, so concurrent builds cannot take every core
TEST(Bm3dRunner, theToolGetsTheThreadsOfABuildThatThePoolHasFree) {
    Bm3dFixture f("runner_bm3d_threads");
    EnvVarGuard threads("DDB_BUILD_THREADS", "3");
    auto threadsArg = [&f]() {
        const auto &args = f.fake.calls.back().args;
        const auto it = std::find(args.begin(), args.end(), "--threads");
        return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::string();
    };
    const auto before = parallel::cpuTokensInUse();
    size_t heldWhileRunning = 0;
    f.fake.effect = [&](const FakeProcessRunner::Call &) {
        heldWhileRunning = parallel::cpuTokensInUse() - before;
        touch(f.output, "bm3d");
    };

    {
        const ScopedProcessConfig pool(R"({"cpuPoolSize": 16})");
        f.run();
        EXPECT_EQ(threadsArg(), "3");
        EXPECT_EQ(heldWhileRunning, 2u) << "the calling thread waits: the child gets it and 2 tokens";
        EXPECT_EQ(parallel::cpuTokensInUse(), before);
    }
    {
        const ScopedProcessConfig pool(R"({"cpuPoolSize": 1})");
        f.run();
        EXPECT_EQ(threadsArg(), "2");
    }
    {
        const ScopedProcessConfig pool(R"({"cpuPoolSize": 0})");
        f.run();
        EXPECT_EQ(threadsArg(), "1");
    }
}

// The cap of a build (BuildOptions::threads) lowers the threads of the tool and the tokens it
// holds; it never raises them above DDB_BUILD_THREADS
TEST(Bm3dRunner, theCapOfABuildLowersTheThreadsOfTheTool) {
    Bm3dFixture f("runner_bm3d_build_cap");
    EnvVarGuard threads("DDB_BUILD_THREADS", "6");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 16})");
    const auto before = parallel::cpuTokensInUse();
    size_t heldWhileRunning = 0;
    f.fake.effect = [&](const FakeProcessRunner::Call &) {
        heldWhileRunning = parallel::cpuTokensInUse() - before;
        touch(f.output, "bm3d");
    };
    const auto threadsArg = [&f]() {
        const auto &args = f.fake.calls.back().args;
        return args.at(1);
    };

    bm3d::runBm3dBuild(f.input, f.output, f.tool, f.fake, BuildOptions::withThreads(2));
    EXPECT_EQ(threadsArg(), "2");
    EXPECT_EQ(heldWhileRunning, 1u);
    EXPECT_EQ(parallel::cpuTokensInUse(), before);

    bm3d::runBm3dBuild(f.input, f.output, f.tool, f.fake, BuildOptions::withThreads(4096));
    EXPECT_EQ(threadsArg(), "6") << "a cap above the budget does not raise it";

    bm3d::runBm3dBuild(f.input, f.output, f.tool, f.fake, BuildOptions());
    EXPECT_EQ(threadsArg(), "6");

    bm3d::runBm3dBuild(f.input, f.output, f.tool, f.fake, BuildOptions::withThreads(1));
    EXPECT_EQ(threadsArg(), "1");
}

TEST(Bm3dRunner, anExitCodeIsAFailureWithTheLastOutput) {
    Bm3dFixture f("runner_bm3d_exit");
    // the reason comes last, after more than the 2000 bytes the message keeps
    f.fake.result =
        FakeProcessRunner::exited(1,
                                  "first line of the progress\n" + std::string(3000, '.') +
                                      "\nbm3d: could not build the model");
    f.fake.effect = nullptr;
    const std::string message = f.failureMessage();
    EXPECT_EQ(message.rfind("Could not build bm3d file for " + io::toUtf8(f.input) + ": ", 0), 0u)
        << message;
    EXPECT_NE(message.find("bm3d exited with code 1: "), std::string::npos) << message;
    EXPECT_NE(message.find("bm3d: could not build the model"), std::string::npos) << message;
    EXPECT_EQ(message.find("first line of the progress"), std::string::npos) << "only the tail";
    EXPECT_LT(message.size(), 2400u);
}

// Exit code 2 is bm3d refusing the model (missing, unreadable or unparseable mesh or texture):
// the build cannot succeed until the input changes, which is an invalid argument of the build and
// no failure of the tool; 1 (the build failed) and 64 (usage) stay plain failures
TEST(Bm3dRunner, anInvalidInputIsToldApartFromAFailedBuild) {
    Bm3dFixture f("runner_bm3d_invalid_input");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::exited(2, "bm3d: model.obj: no vertices");
    try {
        f.run();
        FAIL() << "an invalid input throws";
    } catch (const InvalidArgsException& e) {
        const std::string message = e.what();
        EXPECT_EQ(message.rfind("Could not build bm3d file for " + io::toUtf8(f.input) + ": ", 0),
                  0u)
            << message;
        EXPECT_NE(message.find("bm3d exited with code 2: "), std::string::npos) << message;
        EXPECT_NE(message.find("no vertices"), std::string::npos) << message;
    }

    for (const int code : {1, 64}) {
        f.fake.result = FakeProcessRunner::exited(code, "bm3d: failed");
        try {
            f.run();
            FAIL() << "exit code " << code << " throws";
        } catch (const InvalidArgsException&) {
            ADD_FAILURE() << "exit code " << code << " is not an invalid input";
        } catch (const AppException& e) {
            EXPECT_NE(std::string(e.what()).find("bm3d exited with code " + std::to_string(code)),
                      std::string::npos)
                << e.what();
        }
    }
}

TEST(Bm3dRunner, aSignalIsAFailureThatNamesIt) {
    Bm3dFixture f("runner_bm3d_signal");
    process::Result killed;
    killed.started = true;
    killed.termSignal = 11;
    killed.outputTail = "terminate called after throwing an instance of std::bad_alloc";
    f.fake.result = killed;
    f.fake.effect = nullptr;
    const std::string message = f.failureMessage();
    EXPECT_NE(message.find("killed by signal 11"), std::string::npos) << message;
    EXPECT_NE(message.find("terminate called after throwing an instance of std::bad_alloc"),
              std::string::npos)
        << message;
}

TEST(Bm3dRunner, aTimeoutAndAToolThatCannotStartAreFailures) {
    Bm3dFixture f("runner_bm3d_timeout");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::timedOutAfterIdle();
    EXPECT_NE(f.failureMessage().find("killed after 3600 s without output"), std::string::npos);

    process::Result notStarted;
    notStarted.errorMessage = "Permission denied";
    f.fake.result = notStarted;
    EXPECT_NE(f.failureMessage().find("failed to start: Permission denied"), std::string::npos);
}

TEST(Bm3dRunner, successWithoutTheOutputFileIsAFailure) {
    Bm3dFixture f("runner_bm3d_nooutput");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::exited(0, "bm3d: building");
    const std::string message = f.failureMessage();
    EXPECT_NE(message.find("wrote no output file: " + io::toUtf8(f.output)), std::string::npos) << message;
    EXPECT_NE(message.find("bm3d: building"), std::string::npos) << message;

    // an empty file is no output either
    f.fake.effect = [&](const FakeProcessRunner::Call &) { touch(f.output, ""); };
    EXPECT_NE(f.failureMessage().find("wrote no output file"), std::string::npos);
}

TEST(Bm3dRunner, aMissingToolNamesItAndTheVariableAndStartsNothing) {
    Bm3dFixture f("runner_bm3d_missing");
    f.tool = f.ta.getPath("missing/bm3d");
    const std::string message = f.failureMessage();
    EXPECT_NE(message.find("bm3d"), std::string::npos) << message;
    EXPECT_NE(message.find("DDB_BM3D_PATH"), std::string::npos) << message;
    EXPECT_TRUE(f.fake.calls.empty());
}

TEST(Bm3dRunner, aMissingToolIsLookedUpThroughTheVariable) {
    Bm3dFixture f("runner_bm3d_variable");
    EnvVarGuard guard("DDB_BM3D_PATH", io::toUtf8(f.ta.getPath("missing/bm3d")));
    bm3d::findBm3dBinary(true);
    try {
        bm3d::runBm3dBuild(f.input, f.output, {}, f.fake);
        FAIL() << "a missing tool throws";
    } catch (const AppException &e) {
        EXPECT_NE(std::string(e.what()).find("DDB_BM3D_PATH"), std::string::npos) << e.what();
    }
    EXPECT_TRUE(f.fake.calls.empty());

    setEnv("DDB_BM3D_PATH", io::toUtf8(f.tool));
    bm3d::runBm3dBuild(f.input, f.output, {}, f.fake);
    ASSERT_EQ(f.fake.calls.size(), 1u);
    EXPECT_EQ(f.fake.calls[0].binary, f.tool);
}

TEST(Bm3dRunner, aCanceledChildPropagatesAndReleasesItsCpuTokens) {
    Bm3dFixture f("runner_bm3d_cancel");
    f.fake.effect = [](const FakeProcessRunner::Call &) { throw CanceledException("canceled"); };
    const auto before = parallel::cpuTokensInUse();
    EXPECT_THROW(f.run(), CanceledException);
    EXPECT_EQ(parallel::cpuTokensInUse(), before);
}

// ------------------------------------------------------------------------ Untwine

struct UntwineFixture {
    TestArea ta;
    fs::path tool, outFile, tempDir;
    std::vector<std::string> inputs;
    FakeProcessRunner fake;

    explicit UntwineFixture(const std::string &name) : ta(name, true) {
        tool = fakeTool(ta, "bin/untwine");
        outFile = ta.getPath("out/cloud.copc.laz");
        tempDir = ta.getPath("tmp");
        inputs = {io::toUtf8(ta.getPath("a.laz")), io::toUtf8(ta.getPath("b.laz"))};
        fake.effect = [this](const FakeProcessRunner::Call &) { touch(outFile); };
    }

    ToolRunResult run(const process::Options &options = process::Options()) {
        return untwine::runUntwineResult(inputs, outFile, tempDir, tool, options, fake);
    }
};

TEST(UntwineRunner, buildsTheArgumentsAndTheProgressChannel) {
    UntwineFixture f("runner_untwine_args");
    bool channelFed = false;
    f.fake.effect = [&](const FakeProcessRunner::Call &call) {
        touch(f.outFile);
        // The progress decoder lives only during the run: a message (id 1000, 50 %, "half")
        // and a truncated one are decoded without trouble
        ASSERT_TRUE(static_cast<bool>(call.options.onExtraData));
        const int32_t id = 1000;
        const uint32_t percent = 50, size = 4;
        std::string message(reinterpret_cast<const char *>(&id), 4);
        message.append(reinterpret_cast<const char *>(&percent), 4).append(reinterpret_cast<const char *>(&size), 4);
        message += "half";
        call.options.onExtraData(message.data(), message.size());
        call.options.onExtraData(message.data(), 6);
        channelFed = true;
    };
    const auto r = f.run();
    ASSERT_TRUE(r.succeeded) << r.error;
    EXPECT_TRUE(channelFed);
    ASSERT_EQ(f.fake.calls.size(), 1u);
    const auto &call = f.fake.calls[0];
    EXPECT_EQ(call.binary, f.tool);
    EXPECT_TRUE(hasArgs(call.args, {"--files", f.inputs[0] + "," + f.inputs[1], "--output_dir", io::toUtf8(f.outFile),
                                    "--temp_dir", io::toUtf8(f.tempDir), "--progress_fd",
                                    process::kExtraChannelPlaceholder}));
    EXPECT_TRUE(call.options.extraChannel);
}

// Untwine has no thread option: it runs its own threads whatever a build is capped to, and the
// tokens of its 10 threads (the calling thread is one) say so to the other sections
TEST(UntwineRunner, itHoldsTheTokensOfItsFixedThreadsWhateverTheCapOfTheBuild) {
    UntwineFixture f("runner_untwine_tokens");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 64})");
    EnvVarGuard threads("DDB_BUILD_THREADS", "1");
    const auto before = parallel::cpuTokensInUse();
    size_t heldWhileRunning = 0;
    f.fake.effect = [&](const FakeProcessRunner::Call &) {
        heldWhileRunning = parallel::cpuTokensInUse() - before;
        touch(f.outFile);
    };
    ASSERT_TRUE(f.run().succeeded);
    EXPECT_EQ(heldWhileRunning, 9u) << "10 threads: the calling one and 9 tokens";
    EXPECT_EQ(parallel::cpuTokensInUse(), before);
}

TEST(UntwineRunner, theCallersOptionsAreKept) {
    UntwineFixture f("runner_untwine_options");
    process::Options options;
    options.idleTimeout = std::chrono::seconds(7);
    ASSERT_TRUE(f.run(options).succeeded);
    EXPECT_EQ(f.fake.calls.at(0).options.idleTimeout, std::chrono::seconds(7));
}

TEST(UntwineRunner, withoutATempFolderTheFlagIsLeftOut) {
    UntwineFixture f("runner_untwine_notemp");
    f.tempDir.clear();
    ASSERT_TRUE(f.run().succeeded);
    EXPECT_FALSE(contains(f.fake.calls.at(0).args, "--temp_dir"));
}

TEST(UntwineRunner, anExitCodeIsAFailureAndATimeoutIsRetryable) {
    UntwineFixture f("runner_untwine_failures");
    f.fake.effect = nullptr;

    f.fake.result = FakeProcessRunner::exited(2, "PDAL: bad header");
    auto r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.error, "untwine exited with code 2: PDAL: bad header");
    EXPECT_NO_THROW(r.throwIfRetryable("COPC: "));

    f.fake.result = FakeProcessRunner::timedOutAfterIdle();
    r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_TRUE(r.timedOut);
    try {
        r.throwIfRetryable("COPC: ");
        FAIL() << "a timeout is retryable";
    } catch (const RetryableBuildException &e) {
        EXPECT_EQ(e.reason(), RetryableBuildReason::IdleTimeout);
    }
}

TEST(UntwineRunner, successWithoutTheOutputFileIsAFailure) {
    UntwineFixture f("runner_untwine_nooutput");
    f.fake.effect = nullptr;
    const auto r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_NE(r.error.find("reported success but expected output is missing"), std::string::npos) << r.error;
}

TEST(UntwineRunner, aStaleOutputIsRemovedBeforeTheLaunch) {
    UntwineFixture f("runner_untwine_stale");
    touch(f.outFile, "stale");
    bool existedAtLaunch = true;
    f.fake.effect = [&](const FakeProcessRunner::Call &) { existedAtLaunch = fs::exists(f.outFile); };
    const auto r = f.run();
    EXPECT_FALSE(existedAtLaunch);
    EXPECT_FALSE(r.succeeded);  // the fake wrote nothing
}

TEST(UntwineRunner, aMissingToolOrNoInputNeverStartsAChild) {
    UntwineFixture f("runner_untwine_missing");
    f.tool = f.ta.getPath("missing/untwine");
    auto r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_EQ(r.error, "untwine binary not found");

    f.tool = fakeTool(f.ta, "bin/untwine");
    f.inputs.clear();
    r = f.run();
    EXPECT_EQ(r.error, "no input files provided");
    EXPECT_TRUE(f.fake.calls.empty());
}

TEST(UntwineRunner, aCanceledChildPropagatesAndReleasesItsCpuTokens) {
    UntwineFixture f("runner_untwine_cancel");
    f.fake.effect = [](const FakeProcessRunner::Call &) { throw ShutdownException("shutting down"); };
    const auto before = parallel::cpuTokensInUse();
    EXPECT_THROW(f.run(), CanceledException);
    EXPECT_EQ(parallel::cpuTokensInUse(), before);
}

// ------------------------------------------------------------------------ build-lod

// What build-lod does to the file it is given: writes "<stem>-lod.rad" beside it
fs::path buildLodOutputOf(const fs::path &given) {
    return given.parent_path() / (given.stem().string() + "-lod.rad");
}

struct BuildLodFixture {
    TestArea ta;
    fs::path tool, input, outRad;
    FakeProcessRunner fake;
    // The files the tool was given and wrote, as seen at the launch
    fs::path given, produced;

    explicit BuildLodFixture(const std::string &name) : ta(name, true) {
        tool = fakeTool(ta, "bin/build-lod");
        input = ta.getPath("in/model.spz");
        touch(input);
        outRad = ta.getPath("out/model.rad");
        fake.effect = [this](const FakeProcessRunner::Call &call) {
            given = fs::path(call.args.at(0));
            produced = buildLodOutputOf(given);
            touch(produced, "rad");
        };
    }

    ToolRunResult run(bool quality = true, int maxSh = 1) {
        return buildlod::runBuildLodResult({input, outRad, quality, maxSh, tool}, process::Options(), fake);
    }
};

TEST(BuildLodRunner, buildsTheArgumentsAndMovesTheOutput) {
    EnvVarGuard idle("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS");
    unsetEnv("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS");

    BuildLodFixture f("runner_buildlod_args");
    const auto r = f.run();
    ASSERT_TRUE(r.succeeded) << r.error;
    ASSERT_EQ(f.fake.calls.size(), 1u);
    EXPECT_EQ(f.fake.calls[0].binary, f.tool);
    // the tool is given a staged copy named by us, not the dataset file
    EXPECT_TRUE(hasArgs(f.fake.calls[0].args, {io::toUtf8(f.given), "--quality", "--max-sh=1", "--rad"}));
    EXPECT_EQ(f.given.filename(), "source.spz");
    EXPECT_NE(f.given.parent_path(), f.input.parent_path());
    EXPECT_EQ(f.fake.calls[0].options.idleTimeout, process::kDefaultIdleTimeout);
    EXPECT_TRUE(fs::exists(f.outRad));
    EXPECT_EQ(fs::file_size(f.outRad), 3u) << "the RAD that build-lod wrote is the output";
    EXPECT_FALSE(fs::exists(f.produced));
    EXPECT_FALSE(fs::exists(f.given.parent_path())) << "the scratch folder is gone";
}

TEST(BuildLodRunner, quickModeAndTheShClamp) {
    BuildLodFixture f("runner_buildlod_clamp");
    ASSERT_TRUE(f.run(false, 9).succeeded);
    EXPECT_TRUE(hasArgs(f.fake.calls.at(0).args, {io::toUtf8(f.given), "--quick", "--max-sh=3", "--rad"}));
    ASSERT_TRUE(f.run(true, -2).succeeded);
    EXPECT_TRUE(contains(f.fake.calls.at(1).args, "--max-sh=0"));
}

TEST(BuildLodRunner, theIdleTimeoutComesFromTheEnvironment) {
    BuildLodFixture f("runner_buildlod_idle");
    {
        EnvVarGuard idle("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "90");
        ASSERT_TRUE(f.run().succeeded);
        EXPECT_EQ(f.fake.calls.at(0).options.idleTimeout, std::chrono::seconds(90));
    }
    for (const char *bad : {"0", "-1", "abc", "86401", "1.5"}) {
        EnvVarGuard idle("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", bad);
        fs::remove(f.outRad);
        const auto calls = f.fake.calls.size();
        const auto r = f.run();
        EXPECT_FALSE(r.succeeded) << bad;
        EXPECT_NE(r.error.find("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS"), std::string::npos) << bad;
        EXPECT_EQ(f.fake.calls.size(), calls) << "no launch for " << bad;
    }
    EnvVarGuard top("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "86400");
    ASSERT_TRUE(f.run().succeeded);
    EXPECT_EQ(f.fake.calls.back().options.idleTimeout, std::chrono::hours(24));
}

// The header of a 3DGS PLY of `splats` splats with SH degree 3 (the body is not read)
std::string splatPlyHeader(uint64_t splats) {
    std::string header = "ply\nformat binary_little_endian 1.0\nelement vertex " + std::to_string(splats) + "\n";
    for (const char *name : {"x", "y", "z", "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2",
                             "rot_3", "f_dc_0", "f_dc_1", "f_dc_2"})
        header += std::string("property float ") + name + "\n";
    for (int i = 0; i < 45; ++i)
        header += "property float f_rest_" + std::to_string(i) + "\n";
    return header + "end_header\n";
}

// The 16-byte header of a v4 (NGSP) SPZ of `splats` splats
std::string ngspSpzHeader(uint32_t splats) {
    std::string header = "NGSP";
    const uint32_t fields[] = {4, splats};
    header.append(reinterpret_cast<const char *>(fields), sizeof(fields));
    header += std::string("\x03\0\0\0", 4); // SH degree 3, flags, reserved
    return header;
}

TEST(BuildLodIdleLimit, scalesWithTheSplatsBetweenTheDefaultAndADay) {
    using std::chrono::hours;
    using std::chrono::minutes;
    using std::chrono::seconds;
    // Never under the limit of every other tool
    EXPECT_EQ(buildlod::idleTimeoutForSplats(0), minutes(60));
    EXPECT_EQ(buildlod::idleTimeoutForSplats(1000), minutes(60));
    EXPECT_EQ(buildlod::idleTimeoutForSplats(7'500'000), minutes(60));
    // 8 minutes per million splats, rounded up to a whole second
    EXPECT_EQ(buildlod::idleTimeoutForSplats(10'000'000), minutes(80));
    EXPECT_EQ(buildlod::idleTimeoutForSplats(10'000'001), seconds(4801));
    // The 7.45 GB PLY that was killed after an hour of silence in its pruning
    EXPECT_EQ(buildlod::idleTimeoutForSplats(31'586'621), seconds(15162));
    // Never over a day, however large the count a header gives
    EXPECT_EQ(buildlod::idleTimeoutForSplats(180'000'000), hours(24));
    EXPECT_EQ(buildlod::idleTimeoutForSplats(1'000'000'000), hours(24));
    EXPECT_EQ(buildlod::idleTimeoutForSplats(UINT64_MAX), hours(24));
}

TEST(BuildLodIdleLimit, theCountComesFromTheHeaderOfThePlyOrTheSpz) {
    TestArea ta("buildlod_idle_header", true);
    // The files hold their headers only: the count is not taken from their size
    const fs::path ply = ta.getPath("scene.ply");
    touch(ply, splatPlyHeader(31'586'621));
    EXPECT_EQ(buildlod::idleTimeoutFor(ply), std::chrono::seconds(15162));
    const fs::path upper = ta.getPath("SCENE.PLY");
    touch(upper, splatPlyHeader(20'000'000));
    EXPECT_EQ(buildlod::idleTimeoutFor(upper), std::chrono::minutes(160));
    const fs::path spz = ta.getPath("scene.spz");
    touch(spz, ngspSpzHeader(20'000'000));
    EXPECT_EQ(buildlod::idleTimeoutFor(spz), std::chrono::minutes(160));
    // A small splat keeps the default
    const fs::path small = ta.getPath("small.ply");
    touch(small, splatPlyHeader(100));
    EXPECT_EQ(buildlod::idleTimeoutFor(small), process::kDefaultIdleTimeout);
}

TEST(BuildLodIdleLimit, withoutACountTheSizeOfTheFileIsUsed) {
    TestArea ta("buildlod_idle_size", true);
    // A .sog has no header reader, a point cloud PLY no splat count: their size is used
    const fs::path sog = ta.getPath("bundle.sog");
    touch(sog, std::string("PK\x03\x04rest", 8));
    EXPECT_EQ(buildlod::idleTimeoutFor(sog), process::kDefaultIdleTimeout);
    const fs::path cloud = ta.getPath("cloud.ply");
    touch(cloud, "ply\nformat binary_little_endian 1.0\nelement vertex 99000000\nproperty float x\nproperty float y\n"
                 "property float z\nend_header\n");
    EXPECT_EQ(buildlod::idleTimeoutFor(cloud), process::kDefaultIdleTimeout) << "its count is not a splat count";
    // What cannot be read keeps the default
    EXPECT_EQ(buildlod::idleTimeoutFor(ta.getPath("missing.sog")), process::kDefaultIdleTimeout);
#ifndef _WIN32
    // 236 bytes a splat, as in a PLY of SH degree 3: 15 M splats, 120 minutes (a sparse file)
    fs::resize_file(sog, 236ull * 15'000'000);
    EXPECT_EQ(buildlod::idleTimeoutFor(sog), std::chrono::minutes(120));
#endif
}

TEST(BuildLodIdleLimit, theEnvironmentOverridesTheScaledLimit) {
    BuildLodFixture f("runner_buildlod_idle_scaled");
    f.input = f.ta.getPath("in/scene.ply");
    touch(f.input, splatPlyHeader(31'586'621));
    process::Options options;
    options.idleTimeout = buildlod::idleTimeoutFor(f.input);
    const auto run = [&] {
        fs::remove(f.outRad);
        return buildlod::runBuildLodResult({f.input, f.outRad, true, 3, f.tool}, options, f.fake);
    };
    {
        EnvVarGuard idle("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS");
        unsetEnv("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS");
        ASSERT_TRUE(run().succeeded);
        EXPECT_EQ(f.fake.calls.back().options.idleTimeout, std::chrono::seconds(15162));
    }
    EnvVarGuard idle("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", "600");
    ASSERT_TRUE(run().succeeded);
    EXPECT_EQ(f.fake.calls.back().options.idleTimeout, std::chrono::seconds(600));
}

TEST(BuildLodRunner, aFailureRemovesTheProducedFileAndKeepsTheTimeoutFlag) {
    EnvVarGuard idle("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS");
    unsetEnv("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS");
    BuildLodFixture f("runner_buildlod_failures");

    f.fake.result = FakeProcessRunner::exited(101, "panic: bad ply");
    auto r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.error, "build-lod exited with code 101: panic: bad ply");
    EXPECT_FALSE(fs::exists(f.produced)) << "a partial output is removed";
    EXPECT_FALSE(fs::exists(f.outRad));

    f.fake.result = FakeProcessRunner::timedOutAfterIdle();
    r = f.run();
    EXPECT_TRUE(r.timedOut);
    EXPECT_THROW(r.throwIfRetryable("gsplat: "), RetryableBuildException);
}

TEST(BuildLodRunner, successWithoutTheExpectedOutputIsAFailure) {
    BuildLodFixture f("runner_buildlod_nooutput");
    f.fake.effect = nullptr;
    const auto r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_NE(r.error.find("reported success but expected output is missing"), std::string::npos) << r.error;
}

// build-lod writes beside the file it is given. It is given a staged copy, so the dataset folder
// is never written to or deleted from, and a file there that looks like its output survives.
TEST(BuildLodRunner, theDatasetFolderIsNeverWrittenToOrDeletedFrom) {
    BuildLodFixture f("runner_buildlod_untouched");
    const fs::path userRad = f.input.parent_path() / "model-lod.rad";
    touch(userRad, "the user's own file");
    touch(f.outRad, "stale");
    const auto before = fs::last_write_time(userRad);

    const auto r = f.run();
    ASSERT_TRUE(r.succeeded) << r.error;

    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(f.input.parent_path()))
        names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"model-lod.rad", "model.spz"})) << "nothing added to the dataset folder";
    std::ifstream in(userRad, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "the user's own file");
    EXPECT_EQ(fs::last_write_time(userRad), before);
    EXPECT_EQ(fs::file_size(f.outRad), 3u) << "the stale output is replaced by what build-lod wrote";
}

// build-lod skips an input that ends in "-lod.spz" or "-lod.rad", and takes the extension from the
// last dot of the whole path: the staged name does not depend on the name of the dataset file
TEST(BuildLodRunner, aDatasetFileIsStagedUnderOurNameWhateverItIsCalled) {
    std::vector<std::string> names = {"scene-lod.spz", "scene-lod.rad", "a#b c.ply", "100% done.splat",
                                      "dir.with.dots/noext.ply", "SCENE.PLY"};
#ifndef _WIN32
    // '?' cannot be in a file name on Windows, so no dataset file there has one
    names.push_back("a?b#c.ply");
#endif
    for (const auto &name : names) {
        BuildLodFixture f(std::string("runner_buildlod_names_") + std::to_string(std::hash<std::string>()(name) % 100000));
        f.input = f.ta.getPath(std::string("in/") + name);
        touch(f.input, "splat");
        const auto r = f.run();
        ASSERT_TRUE(r.succeeded) << name << ": " << r.error;
        EXPECT_EQ(f.given.stem(), "source") << name;
        EXPECT_EQ(f.given.extension(), f.input.extension()) << name;
        EXPECT_EQ(f.given.string().find_first_of("?#%"), std::string::npos) << name;
    }
}

// The staged file is the dataset file itself, linked or copied: the tool reads its content
TEST(BuildLodRunner, theStagedInputHasTheContentOfTheDatasetFile) {
    BuildLodFixture f("runner_buildlod_content");
    touch(f.input, "the splats");
    std::string seen;
    f.fake.effect = [&](const FakeProcessRunner::Call &call) {
        std::ifstream in(call.args.at(0), std::ios::binary);
        seen.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        touch(buildLodOutputOf(call.args.at(0)), "rad");
    };
    ASSERT_TRUE(f.run().succeeded);
    EXPECT_EQ(seen, "the splats");
    EXPECT_TRUE(fs::exists(f.input)) << "the dataset file stays";
}

#ifndef _WIN32
// A dataset file can be a relative symbolic link to a file elsewhere: what build-lod is given is the
// file it names, not a link that names another file, or none, from the scratch folder
TEST(BuildLodRunner, aDatasetFileThatIsARelativeLinkIsStagedAsTheFileItNames) {
    BuildLodFixture f("runner_buildlod_relative_link");
    touch(f.ta.getPath("elsewhere/model.spz"), "the splats");
    fs::remove(f.input);
    fs::create_symlink(fs::path("..") / "elsewhere" / "model.spz", f.input);
    f.outRad = f.ta.getPath("deeper/build/out/model.rad"); // the scratch folder is not as deep as the link
    std::string seen;
    f.fake.effect = [&](const FakeProcessRunner::Call &call) {
        std::ifstream in(call.args.at(0), std::ios::binary);
        seen.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        touch(buildLodOutputOf(call.args.at(0)), "rad");
    };
    ASSERT_TRUE(f.run().succeeded);
    EXPECT_EQ(seen, "the splats");
    EXPECT_TRUE(fs::is_symlink(f.input)) << "the dataset link stays";
}
#endif

// The scratch folder is beside the folder of the output, named after it, and not in it: the folder
// of the output is a build's scratch folder, published as it is, and a scratch folder whose removal
// fails stays out of it (beside a build folder its name keeps "-temp-", for the orphan cleanup)
TEST(BuildLodRunner, theScratchFolderIsBesideTheOutputFolderNotInIt) {
    BuildLodFixture f("runner_buildlod_beside");
    f.outRad = f.ta.getPath("gsplat-temp-0123456789abcdef/model.rad");
    ASSERT_TRUE(f.run().succeeded);
    const fs::path outFolder = fs::absolute(f.outRad.parent_path()).lexically_normal();
    EXPECT_EQ(f.given.parent_path().parent_path(), outFolder.parent_path());
    EXPECT_EQ(f.given.parent_path().filename().string().rfind("gsplat-temp-0123456789abcdef-build-lod-", 0), 0u)
        << f.given;
    EXPECT_FALSE(fs::exists(f.given.parent_path()));

#ifndef _WIN32
    if (geteuid() == 0)
        GTEST_SKIP() << "root removes a folder whatever its permissions";
    fs::path locked;
    f.fake.effect = [&](const FakeProcessRunner::Call &call) {
        f.given = fs::path(call.args.at(0));
        touch(buildLodOutputOf(f.given), "rad");
        locked = f.given.parent_path() / "locked";
        touch(locked / "file");
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    };
    const auto r = f.run();
    std::vector<std::string> published;
    for (const auto &e : fs::directory_iterator(f.outRad.parent_path()))
        published.push_back(e.path().filename().string());
    const bool leftOver = fs::exists(locked);
    fs::permissions(locked, fs::perms::owner_all);
    fs::remove_all(locked.parent_path());
    ASSERT_TRUE(r.succeeded) << r.error;
    EXPECT_TRUE(leftOver) << "the scratch folder was expected to resist its removal";
    EXPECT_EQ(published, std::vector<std::string>{"model.rad"});
#endif
}

// Nothing but the RAD remains in the folder of the output, whether the tool worked, failed or
// wrote nothing
TEST(BuildLodRunner, theOutputFolderHoldsOnlyTheRadWhateverHappened) {
    BuildLodFixture f("runner_buildlod_scratch");
    const auto remaining = [&] {
        std::vector<std::string> names;
        for (const auto &e : fs::directory_iterator(f.outRad.parent_path()))
            names.push_back(e.path().filename().string());
        return names;
    };

    ASSERT_TRUE(f.run().succeeded);
    EXPECT_EQ(remaining(), std::vector<std::string>{"model.rad"});

    fs::remove(f.outRad);
    f.fake.result = FakeProcessRunner::exited(101, "panic");
    EXPECT_FALSE(f.run().succeeded);
    EXPECT_TRUE(remaining().empty());

    f.fake.result = FakeProcessRunner::succeeded();
    f.fake.effect = nullptr;
    EXPECT_FALSE(f.run().succeeded);
    EXPECT_TRUE(remaining().empty());

    f.fake.effect = [](const FakeProcessRunner::Call &) { throw CanceledException("canceled"); };
    EXPECT_THROW(f.run(), CanceledException);
    EXPECT_TRUE(remaining().empty()) << "a canceled child leaves nothing either";
}

// build-lod exits 0 after a decode failure and says why on its output: the error carries it
TEST(BuildLodRunner, anExitZeroWithoutOutputCarriesWhatTheToolPrinted) {
    BuildLodFixture f("runner_buildlod_reason");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::exited(0, "*** Processing: x\nDecoding failed: invalid splats");
    auto r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_NE(r.error.find("Decoding failed: invalid splats"), std::string::npos) << r.error;

    // an empty RAD is no output either
    f.fake.effect = [&](const FakeProcessRunner::Call &call) { touch(buildLodOutputOf(call.args.at(0)), ""); };
    r = f.run();
    EXPECT_FALSE(r.succeeded);
    EXPECT_NE(r.error.find("Decoding failed: invalid splats"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("is empty"), std::string::npos) << r.error;
    EXPECT_FALSE(fs::exists(f.outRad));
}

TEST(BuildLodRunner, theChildHoldsACpuTokenWhileItRuns) {
    BuildLodFixture f("runner_buildlod_tokens");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 16})");
    const auto before = parallel::cpuTokensInUse();
    size_t heldWhileRunning = 0;
    f.fake.effect = [&](const FakeProcessRunner::Call &call) {
        heldWhileRunning = parallel::cpuTokensInUse() - before;
        touch(buildLodOutputOf(call.args.at(0)), "rad");
    };
    ASSERT_TRUE(f.run().succeeded);
    EXPECT_EQ(heldWhileRunning, 1u);
    EXPECT_EQ(parallel::cpuTokensInUse(), before) << "given back";
}

// ------------------------------------------------------------------------ tool_run_support

TEST(ToolRunSupport, anExitZeroWithoutOutputIsAnErrorWithTheTailOfTheOutput) {
    TestArea ta("runner_support_nooutput", true);
    FakeProcessRunner fake;
    const fs::path out = ta.getPath("out/result.bin");
    const tools::ToolRun run{"sometool", fakeTool(ta, "bin/sometool"), {"a"}, process::Options(), out, 40};
    fake.result = FakeProcessRunner::exited(0, std::string(200, 'x') + "\nthe reason it printed");

    auto outcome = tools::runToolProducingFile(run, fake);
    EXPECT_FALSE(outcome.result.succeeded);
    EXPECT_FALSE(outcome.result.timedOut);
    EXPECT_EQ(outcome.result.error.rfind("sometool reported success but expected output is missing: " + io::toUtf8(out), 0), 0u)
        << outcome.result.error;
    EXPECT_NE(outcome.result.error.find("the reason it printed"), std::string::npos) << outcome.result.error;
    EXPECT_EQ(outcome.result.error.find(std::string(100, 'x')), std::string::npos) << "only the tail";
    EXPECT_TRUE(outcome.process.success()) << "the caller can still read how the child ended";

    touch(out, "");
    outcome = tools::runToolProducingFile(run, fake);
    EXPECT_FALSE(outcome.result.succeeded);
    EXPECT_NE(outcome.result.error.find("the reason it printed"), std::string::npos) << outcome.result.error;

    fs::remove(out);
    fs::create_directories(out);
    outcome = tools::runToolProducingFile(run, fake);
    EXPECT_FALSE(outcome.result.succeeded) << "a folder is no output";
}

TEST(ToolRunSupport, aNonEmptyOutputSucceedsAndAFailedChildKeepsItsOwnError) {
    TestArea ta("runner_support_ok", true);
    FakeProcessRunner fake;
    const fs::path out = ta.getPath("out/result.bin");
    const tools::ToolRun run{"sometool", fakeTool(ta, "bin/sometool"), {}, process::Options(), out, 100};
    fake.effect = [&](const FakeProcessRunner::Call &) { touch(out, "data"); };
    EXPECT_TRUE(tools::runToolProducingFile(run, fake).result.succeeded);

    fake.result = FakeProcessRunner::exited(7, "boom");
    auto outcome = tools::runToolProducingFile(run, fake);
    EXPECT_EQ(outcome.result.error, "sometool exited with code 7: boom");
    EXPECT_EQ(outcome.process.exitCode, 7);

    fake.result = FakeProcessRunner::timedOutAfterIdle();
    EXPECT_TRUE(tools::runToolProducingFile(run, fake).result.timedOut);
}

TEST(BuildLodRunner, aMissingToolOrInputNeverStartsAChild) {
    BuildLodFixture f("runner_buildlod_missing");
    f.tool = f.ta.getPath("missing/build-lod");
    auto r = f.run();
    EXPECT_EQ(r.error, "build-lod binary not found");

    f.tool = fakeTool(f.ta, "bin/build-lod");
    fs::remove(f.input);
    r = f.run();
    EXPECT_NE(r.error.find("build-lod input does not exist"), std::string::npos) << r.error;
    EXPECT_TRUE(f.fake.calls.empty());
}

TEST(BuildLodRunner, aCanceledChildPropagates) {
    BuildLodFixture f("runner_buildlod_cancel");
    f.fake.effect = [](const FakeProcessRunner::Call &) { throw CanceledException("canceled"); };
    EXPECT_THROW(f.run(), CanceledException);
}

// ------------------------------------------------------------------------ dwg2dxf

TEST(Dwg2DxfRunner, theConverterStartsTheChildThroughTheSeam) {
    TestArea ta("runner_dwg2dxf", true);
    const fs::path tool = fakeTool(ta, "bin/dwg2dxf");
    const fs::path dwg = ta.getPath("drawing.dwg");
    touch(dwg, "AC1032 not really a drawing");
    const fs::path dxf = ta.getPath("drawing.dxf");
    EnvVarGuard maxSeconds("DDB_DWG2DXF_MAX_SECONDS", "45");

    FakeProcessRunner fake;
    fake.result = FakeProcessRunner::exited(1, "Error: unsupported object\nsecond line");
    const cad::LibreDwgConverter converter(tool, fake);
    try {
        converter.toDxf(dwg, dxf);
        FAIL() << "a failed converter throws";
    } catch (const AppException &e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("DWG conversion failed (AC1032 (R2018)): exited with code 1: Error: unsupported object"),
                  std::string::npos)
            << message;
        EXPECT_EQ(message.find('\n'), std::string::npos) << "one line";
    }
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0].binary, tool);
    EXPECT_TRUE(hasArgs(fake.calls[0].args, {"-y", "-o", io::toUtf8(dxf), io::toUtf8(dwg)}));
    EXPECT_EQ(fake.calls[0].options.maxRuntime, std::chrono::seconds(45));
    EXPECT_EQ(fake.calls[0].options.idleTimeout, std::chrono::seconds(60));
}

TEST(Dwg2DxfRunner, aConverterThatExitsZeroWithoutAReadableDxfIsAFailure) {
    TestArea ta("runner_dwg2dxf_unreadable", true);
    const fs::path tool = fakeTool(ta, "bin/dwg2dxf");
    const fs::path dwg = ta.getPath("drawing.dwg");
    touch(dwg, "AC1032 not really a drawing");
    const fs::path dxf = ta.getPath("drawing.dxf");

    FakeProcessRunner fake;
    fake.effect = [&](const FakeProcessRunner::Call &) { touch(dxf, "this is not a DXF"); };
    const cad::LibreDwgConverter converter(tool, fake);
    try {
        converter.toDxf(dwg, dxf);
        FAIL() << "exit code 0 is not trusted";
    } catch (const AppException &e) {
        EXPECT_NE(std::string(e.what()).find("cannot be read"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(dxf));
}

TEST(Dwg2DxfRunner, aMissingConverterNamesTheVariableAndStartsNothing) {
    TestArea ta("runner_dwg2dxf_missing", true);
    const fs::path dwg = ta.getPath("drawing.dwg");
    touch(dwg, "AC1032 not really a drawing");

    FakeProcessRunner fake;
    const cad::LibreDwgConverter converter(ta.getPath("missing/dwg2dxf"), fake);
    try {
        converter.toDxf(dwg, ta.getPath("drawing.dxf"));
        FAIL() << "a missing converter throws";
    } catch (const AppException &e) {
        EXPECT_NE(std::string(e.what()).find("DDB_DWG2DXF_PATH"), std::string::npos) << e.what();
    }
    EXPECT_TRUE(fake.calls.empty());
}

TEST(Dwg2DxfRunner, aCanceledConversionPropagates) {
    TestArea ta("runner_dwg2dxf_cancel", true);
    const fs::path tool = fakeTool(ta, "bin/dwg2dxf");
    const fs::path dwg = ta.getPath("drawing.dwg");
    touch(dwg, "AC1032 not really a drawing");

    FakeProcessRunner fake;
    fake.effect = [](const FakeProcessRunner::Call &) { throw CanceledException("canceled"); };
    const cad::LibreDwgConverter converter(tool, fake);
    EXPECT_THROW(converter.toDxf(dwg, ta.getPath("drawing.dxf")), CanceledException);
}

}  // namespace
