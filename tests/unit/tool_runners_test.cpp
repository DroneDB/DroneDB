/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The environment integer parser, the cached tool locator and the tool runners, driven
// through a fake process runner: arguments, options, exit-code / timeout / cancel mapping and a
// missing tool, without running any tool.

#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "buildlod_runner.h"
#include "cad.h"
#include "env.h"
#include "exceptions.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "nexus_runner.h"
#include "obj2tiles_runner.h"
#include "parallel.h"
#include "process_runner.h"
#include "test.h"
#include "testarea.h"
#include "tool_locator.h"
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
    for (const auto &[var, find] :
         std::vector<std::pair<const char *, std::function<fs::path()>>>{
             {"DDB_OBJ2TILES_PATH", [] { return obj2tiles::findObj2TilesBinary(true); }},
             {"DDB_UNTWINE_PATH", [] { return untwine::findUntwineBinary(true); }},
             {"DDB_BUILDLOD_PATH", [] { return buildlod::findBuildLodBinary(true); }},
             {"DDB_DWG2DXF_PATH", [] { return cad::findDwg2DxfBinary(true); }},
             {"DDB_NEXUS_PATH", [] { return nexus::findNexusBinary(true); }}}) {
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

// ------------------------------------------------------------------------ ddb-nexus

struct NexusFixture {
    TestArea ta;
    fs::path tool;
    fs::path input;
    fs::path output;
    FakeProcessRunner fake;

    explicit NexusFixture(const std::string &name)
        : ta(name, true),
          tool(fakeTool(ta, "bin/ddb-nexus")),
          input(ta.getPath("model.obj")),
          output(ta.getPath("out/model.nxz")) {
        touch(input);
        // the tool writes its output unless a test replaces this
        fake.effect = [this](const FakeProcessRunner::Call &) { touch(output, "nxz"); };
    }

    void run() { nexus::runNexusBuild(input, output, tool, fake); }

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

TEST(NexusRunner, runsTheToolOnTheInputAndTheOutput) {
    NexusFixture f("runner_nexus");
    const ScopedProcessConfig pool(R"({"cpuPoolSize": 16})");
    EnvVarGuard threads("DDB_BUILD_THREADS", "1");
    f.run();
    ASSERT_EQ(f.fake.calls.size(), 1u);
    EXPECT_EQ(f.fake.calls[0].binary, f.tool);
    EXPECT_TRUE(hasArgs(f.fake.calls[0].args, {"--threads", "1", io::toUtf8(f.input), io::toUtf8(f.output)}));
    EXPECT_TRUE(fs::exists(f.output));
}

// The tool keeps busy the threads one build may use (DDB_BUILD_THREADS), as many as the CPU pool
// has free, and their tokens are held while it runs: a Nexus build counts against the same
// budget as the raster builds and the other tools, so concurrent builds cannot take every core
TEST(NexusRunner, theToolGetsTheThreadsOfABuildThatThePoolHasFree) {
    NexusFixture f("runner_nexus_threads");
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
        touch(f.output, "nxz");
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

TEST(NexusRunner, anExitCodeIsAFailureWithTheLastOutput) {
    NexusFixture f("runner_nexus_exit");
    // the reason comes last, after more than the 2000 bytes the message keeps
    f.fake.result = FakeProcessRunner::exited(
        1, "first line of the progress\n" + std::string(3000, '.') + "\nddb-nexus: could not build the model");
    f.fake.effect = nullptr;
    const std::string message = f.failureMessage();
    EXPECT_EQ(message.rfind("Could not build nexus file for " + io::toUtf8(f.input) + ": ", 0), 0u) << message;
    EXPECT_NE(message.find("ddb-nexus exited with code 1: "), std::string::npos) << message;
    EXPECT_NE(message.find("ddb-nexus: could not build the model"), std::string::npos) << message;
    EXPECT_EQ(message.find("first line of the progress"), std::string::npos) << "only the tail";
    EXPECT_LT(message.size(), 2400u);
}

TEST(NexusRunner, aSignalIsAFailureThatNamesIt) {
    NexusFixture f("runner_nexus_signal");
    process::Result killed;
    killed.started = true;
    killed.termSignal = 11;
    killed.outputTail = "QFSFileEngine::map: error";
    f.fake.result = killed;
    f.fake.effect = nullptr;
    const std::string message = f.failureMessage();
    EXPECT_NE(message.find("killed by signal 11"), std::string::npos) << message;
    EXPECT_NE(message.find("QFSFileEngine::map: error"), std::string::npos) << message;
}

TEST(NexusRunner, aTimeoutAndAToolThatCannotStartAreFailures) {
    NexusFixture f("runner_nexus_timeout");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::timedOutAfterIdle();
    EXPECT_NE(f.failureMessage().find("killed after 3600 s without output"), std::string::npos);

    process::Result notStarted;
    notStarted.errorMessage = "Permission denied";
    f.fake.result = notStarted;
    EXPECT_NE(f.failureMessage().find("failed to start: Permission denied"), std::string::npos);
}

TEST(NexusRunner, successWithoutTheOutputFileIsAFailure) {
    NexusFixture f("runner_nexus_nooutput");
    f.fake.effect = nullptr;
    f.fake.result = FakeProcessRunner::exited(0, "ddb-nexus: building");
    const std::string message = f.failureMessage();
    EXPECT_NE(message.find("wrote no output file " + io::toUtf8(f.output)), std::string::npos) << message;
    EXPECT_NE(message.find("ddb-nexus: building"), std::string::npos) << message;

    // an empty file is no output either
    f.fake.effect = [&](const FakeProcessRunner::Call &) { touch(f.output, ""); };
    EXPECT_NE(f.failureMessage().find("wrote no output file"), std::string::npos);
}

TEST(NexusRunner, aMissingToolNamesItAndTheVariableAndStartsNothing) {
    NexusFixture f("runner_nexus_missing");
    f.tool = f.ta.getPath("missing/ddb-nexus");
    const std::string message = f.failureMessage();
    EXPECT_NE(message.find("ddb-nexus"), std::string::npos) << message;
    EXPECT_NE(message.find("DDB_NEXUS_PATH"), std::string::npos) << message;
    EXPECT_TRUE(f.fake.calls.empty());
}

TEST(NexusRunner, aMissingToolIsLookedUpThroughTheVariable) {
    NexusFixture f("runner_nexus_variable");
    EnvVarGuard guard("DDB_NEXUS_PATH", io::toUtf8(f.ta.getPath("missing/ddb-nexus")));
    nexus::findNexusBinary(true);
    try {
        nexus::runNexusBuild(f.input, f.output, {}, f.fake);
        FAIL() << "a missing tool throws";
    } catch (const AppException &e) {
        EXPECT_NE(std::string(e.what()).find("DDB_NEXUS_PATH"), std::string::npos) << e.what();
    }
    EXPECT_TRUE(f.fake.calls.empty());

    setEnv("DDB_NEXUS_PATH", io::toUtf8(f.tool));
    nexus::runNexusBuild(f.input, f.output, {}, f.fake);
    ASSERT_EQ(f.fake.calls.size(), 1u);
    EXPECT_EQ(f.fake.calls[0].binary, f.tool);
}

TEST(NexusRunner, aCanceledChildPropagatesAndReleasesItsCpuTokens) {
    NexusFixture f("runner_nexus_cancel");
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
    EXPECT_NE(r.error.find("reported success but output file is missing"), std::string::npos) << r.error;
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

struct BuildLodFixture {
    TestArea ta;
    fs::path tool, input, outRad, produced;
    FakeProcessRunner fake;

    explicit BuildLodFixture(const std::string &name) : ta(name, true) {
        tool = fakeTool(ta, "bin/build-lod");
        input = ta.getPath("in/model.spz");
        touch(input);
        outRad = ta.getPath("out/model.rad");
        produced = ta.getPath("in/model-lod.rad");
        // build-lod writes <stem>-lod.rad next to its input
        fake.effect = [this](const FakeProcessRunner::Call &) { touch(produced, "rad"); };
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
    EXPECT_TRUE(hasArgs(f.fake.calls[0].args, {io::toUtf8(f.input), "--quality", "--max-sh=1", "--rad"}));
    EXPECT_EQ(f.fake.calls[0].options.idleTimeout, process::kDefaultIdleTimeout);
    EXPECT_TRUE(fs::exists(f.outRad));
    EXPECT_FALSE(fs::exists(f.produced));
}

TEST(BuildLodRunner, quickModeAndTheShClamp) {
    BuildLodFixture f("runner_buildlod_clamp");
    ASSERT_TRUE(f.run(false, 9).succeeded);
    EXPECT_TRUE(hasArgs(f.fake.calls.at(0).args, {io::toUtf8(f.input), "--quick", "--max-sh=3", "--rad"}));
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

TEST(BuildLodRunner, aStaleOutputIsRemovedBeforeTheLaunch) {
    BuildLodFixture f("runner_buildlod_stale");
    touch(f.produced, "stale");
    touch(f.outRad, "stale");
    bool staleAtLaunch = true;
    f.fake.effect = [&](const FakeProcessRunner::Call &) {
        staleAtLaunch = fs::exists(f.produced) || fs::exists(f.outRad);
    };
    f.run();
    EXPECT_FALSE(staleAtLaunch);
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
