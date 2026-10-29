/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// How the real gdal-util program confines its workers (apps/gdal-util/PROTOCOL.md, README.md): the
// report of the layers, the three modes, what a worker may read and write, the limits of each
// operation and the errors they end in. The processes are the ones built next to the test binary;
// what a worker does to itself is read from /proc while the worker waits on a named pipe, so
// nothing here is a sleep and nothing is a test-only switch of the program. The layers themselves
// (what the filter refuses, what Landlock denies) are tested by sandbox_test.cpp; this file tests
// that the program applies them to the right folders and with the right limits.
//
// A layer the host does not offer skips the tests that need it, with the reason. A host without
// Landlock is built by starting the program under a filter that answers ENOSYS to the Landlock
// calls, which is what such a kernel does.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "contract_fixtures.h"
#include "cad.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "process_runner.h"
#include "raster_helper.h"
#include "raster_protocol.h"
#include "raster_test_utils.h"
#include "sandbox/sandbox.h"
#include "testarea.h"
#include "tool_locator.h"

namespace
{
    using namespace ddb;
    using process::Session;
    using std::chrono::seconds;

    constexpr seconds kPatience(60);
    constexpr const char *kToolName = "gdal-util";
    constexpr const char *kToolEnv = "DDB_GDAL_UTIL_PATH";
    constexpr const char *kSandboxEnv = "DDB_GDAL_SANDBOX";

    Session::Clock::time_point patiently() { return Session::Clock::now() + kPatience; }

    fs::path tool() { return tools::findTool(kToolName, kToolEnv); }

    using Environment = std::vector<std::pair<std::string, std::string>>;

    void writeFile(const fs::path &file, const std::string &content)
    {
        std::ofstream out(file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    // Whether this host offers a layer (the library says, without restricting the test process)
    bool hostOffers(const std::string &layer)
    {
        for (const auto &state : sandbox::probe().layers)
            if (state.name == layer)
                return state.active;
        return false;
    }

    const char *kNeedsLandlock = "this kernel offers no Landlock";
    const char *kNeedsSeccomp = "this kernel offers no seccomp";

    // ------------------------------------------------------------------ running the program

    struct ToolRun
    {
        std::vector<std::string> lines; // stdout
        std::string stderrText;
        bool exited = false;
        int exitCode = -1;
    };

    // The program run to its end: the lines are written to its stdin, which is then closed
    ToolRun runTool(const std::vector<std::string> &args, const std::vector<std::string> &input = {},
                    const Environment &environment = {})
    {
        ToolRun run;
        Session session;
        process::SessionOptions options;
        options.envOverrides = environment;
        if (!session.start(tool(), args, options))
        {
            ADD_FAILURE() << session.startError();
            return run;
        }
        for (const auto &line : input)
            EXPECT_EQ(session.writeLine(line, patiently()), Session::WriteStatus::Ok);
        session.closeStdin();
        for (;;)
        {
            const auto line = session.readLine(patiently(), raster::kMaxLineBytes * 2);
            if (line.status != Session::ReadStatus::Line)
            {
                EXPECT_EQ(line.status, Session::ReadStatus::Eof);
                if (!line.line.empty())
                    run.lines.push_back(line.line);
                break;
            }
            run.lines.push_back(line.line);
        }
        EXPECT_TRUE(session.wait(patiently()));
        run.exited = session.exitStatus().exited;
        run.exitCode = session.exitStatus().exitCode;
        run.stderrText = session.stderrTail();
        return run;
    }

    // A running `--serve`, or any program that speaks the protocol
    class Server
    {
      public:
        explicit Server(const Environment &environment = {}, uint64_t maxMemoryBytes = 0)
        {
            process::SessionOptions options;
            options.envOverrides = environment;
            options.maxMemoryBytes = maxMemoryBytes;
            started_ = session_.start(tool(), {"--serve"}, options);
            EXPECT_TRUE(started_) << session_.startError();
        }

        // A server started through the shell, which limits it first (ulimit)
        Server(const std::string &shellLimit, const Environment &environment)
        {
            process::SessionOptions options;
            options.envOverrides = environment;
            const std::string script = shellLimit + "; exec \"$0\" --serve";
            started_ = session_.start("/bin/sh", {"-c", script, io::toUtf8(tool())}, options);
            EXPECT_TRUE(started_) << session_.startError();
        }

        void send(const raster::Request &request)
        {
            ASSERT_EQ(session_.writeLine(raster::encodeRequest(request), patiently()), Session::WriteStatus::Ok);
        }

        raster::Response receive()
        {
            const auto reply = session_.readLine(patiently(), raster::kMaxLineBytes * 2);
            EXPECT_EQ(reply.status, Session::ReadStatus::Line) << session_.stderrTail();
            try
            {
                return raster::decodeResponse(reply.line);
            }
            catch (const raster::ProtocolError &e)
            {
                ADD_FAILURE() << "not a response (" << e.what() << "): " << reply.line.substr(0, 300);
                return raster::Response();
            }
        }

        raster::Response ask(const raster::Request &request)
        {
            send(request);
            return receive();
        }

        Session &session() { return session_; }

      private:
        Session session_;
        bool started_ = false;
    };

    raster::Request request(int64_t id, const char *op, const fs::path &path, const fs::path &out = {},
                            const json &params = json::object())
    {
        raster::Request result;
        result.id = id;
        result.op = op;
        result.path = path;
        result.out = out;
        result.params = params;
        return result;
    }

    raster::Request probeRequest(int64_t id, const fs::path &path, const raster::ProbeParams &params = {})
    {
        return request(id, raster::op::kProbe, path, {}, raster::toJson(params));
    }

    raster::ProbeParams vectorParams()
    {
        raster::ProbeParams params;
        params.kind = raster::ProbeKind::Vector;
        return params;
    }

    // ------------------------------------------------------------------ a host without Landlock

#ifdef __linux__
    struct Captured
    {
        std::string out;
        std::string err;
        int exitCode = -1;
        bool timedOut = false;
    };

    // The landlock calls of the kernel (the numbers are the same on every architecture)
    constexpr unsigned kLandlockCalls[] = {444, 445, 446};

    // The program run under a seccomp filter that answers ENOSYS to the Landlock calls: for the
    // program this is a kernel without Landlock. The filter is inherited by every process the
    // program starts, its workers included, and the program's own filter stacks on top of it.
    Captured runWithoutLandlock(const std::vector<std::string> &args, const Environment &environment,
                                const std::string &input)
    {
        Captured captured;

        std::vector<std::string> argvText = {io::toUtf8(tool())};
        argvText.insert(argvText.end(), args.begin(), args.end());
        std::vector<char *> argv;
        for (auto &text : argvText)
            argv.push_back(text.data());
        argv.push_back(nullptr);

        std::map<std::string, std::string> merged;
        for (char **entry = environ; *entry != nullptr; ++entry)
        {
            const std::string text = *entry;
            const size_t equals = text.find('=');
            if (equals != std::string::npos)
                merged[text.substr(0, equals)] = text.substr(equals + 1);
        }
        for (const auto &[name, value] : environment)
            merged[name] = value;
        std::vector<std::string> envText;
        for (const auto &[name, value] : merged)
            envText.push_back(name + "=" + value);
        std::vector<char *> envp;
        for (auto &text : envText)
            envp.push_back(text.data());
        envp.push_back(nullptr);

        // load the call number, answer ENOSYS to the three, allow the rest
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kLandlockCalls[0], 3, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kLandlockCalls[1], 2, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kLandlockCalls[2], 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        };
        sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};

        int in[2], out[2], err[2];
        if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0)
        {
            ADD_FAILURE() << "cannot make pipes";
            return captured;
        }

        const pid_t pid = fork();
        if (pid == 0)
        {
            // Only calls that are safe between fork and exec
            dup2(in[0], STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            dup2(err[1], STDERR_FILENO);
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
                _exit(126);
            const std::string path = argvText[0];
            execve(path.c_str(), argv.data(), envp.data());
            _exit(127);
        }
        close(in[0]);
        close(out[1]);
        close(err[1]);
        if (pid < 0)
        {
            ADD_FAILURE() << "cannot fork";
            return captured;
        }

        // The program may leave before it reads its input (it refuses to start)
        struct sigaction ignore{}, previous{};
        ignore.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &ignore, &previous);
        size_t written = 0;
        while (written < input.size())
        {
            const ssize_t count = write(in[1], input.data() + written, input.size() - written);
            if (count <= 0)
                break;
            written += static_cast<size_t>(count);
        }
        close(in[1]);
        sigaction(SIGPIPE, &previous, nullptr);

        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
        std::string *sinks[2] = {&captured.out, &captured.err};
        int open = 2;
        while (open > 0)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
            {
                captured.timedOut = true;
                kill(pid, SIGKILL);
                break;
            }
            if (poll(fds, 2, static_cast<int>(left.count())) < 0)
                break;
            for (int i = 0; i < 2; i++)
            {
                if (fds[i].fd < 0 || (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    continue;
                char buffer[4096];
                const ssize_t count = read(fds[i].fd, buffer, sizeof(buffer));
                if (count > 0)
                    sinks[i]->append(buffer, static_cast<size_t>(count));
                else
                {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    open--;
                }
            }
        }
        for (auto &descriptor : fds)
            if (descriptor.fd >= 0)
                close(descriptor.fd);

        int status = 0;
        waitpid(pid, &status, 0);
        captured.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return captured;
    }

    size_t countOf(const std::string &text, const std::string &needle)
    {
        size_t count = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
            count++;
        return count;
    }
#endif

    // ------------------------------------------------------------------ the report

    TEST(GdalUtilSandbox, theReportNamesTheModeAndEveryLayerTheHostOffers)
    {
        const ToolRun run = runTool({"--sandbox-report"}, {}, {{kSandboxEnv, ""}});
        EXPECT_EQ(run.exitCode, 0);
        ASSERT_GE(run.lines.size(), 2u) << run.stderrText;
        EXPECT_EQ(run.lines[0], "mode: best-effort");

        // One line per layer, active or missing as the library finds this host
        const sandbox::Report expected = sandbox::probe();
        ASSERT_EQ(run.lines.size(), expected.layers.size() + 1);
        for (size_t i = 0; i < expected.layers.size(); i++)
        {
            const std::string &line = run.lines[i + 1];
            EXPECT_EQ(line.rfind(expected.layers[i].name + ": " + (expected.layers[i].active ? "active" : "missing"), 0), 0u)
                << line;
        }
#ifdef __linux__
        // On a kernel that has them the three layers that confine a worker are active
        if (hostOffers("seccomp") && hostOffers("landlock"))
        {
            for (const char *layer : {"rlimits", "seccomp", "landlock"})
                EXPECT_NE(std::find_if(run.lines.begin(), run.lines.end(),
                                       [&](const std::string &line) { return line.rfind(std::string(layer) + ": active", 0) == 0; }),
                          run.lines.end())
                    << layer;
        }
#endif
    }

    TEST(GdalUtilSandbox, theReportSaysTheModeOfTheEnvironment)
    {
        EXPECT_EQ(runTool({"--sandbox-report"}, {}, {{kSandboxEnv, "strict"}}).lines.at(0), "mode: strict");
        EXPECT_EQ(runTool({"--sandbox-report"}, {}, {{kSandboxEnv, "off"}}).lines.at(0), "mode: off");
        EXPECT_EQ(runTool({"--sandbox-report"}, {}, {{kSandboxEnv, " Best-Effort "}}).lines.at(0), "mode: best-effort");
        // A value nobody knows is best-effort
        EXPECT_EQ(runTool({"--sandbox-report"}, {}, {{kSandboxEnv, "paranoid"}}).lines.at(0), "mode: best-effort");
    }

    TEST(GdalUtilSandbox, aServerNamesAnUnknownModeOnceAndServes)
    {
        const ToolRun run = runTool({"--serve"}, {}, {{kSandboxEnv, "paranoid"}});
        EXPECT_EQ(run.exitCode, 0);
        EXPECT_NE(run.stderrText.find("paranoid"), std::string::npos) << run.stderrText;
    }

#ifdef __linux__
    // ------------------------------------------------------------------ the three modes

    const char *kProbeLine = R"({"id":1,"op":"probe","path":"/nonexistent/none.tif","params":{"kind":"raster"}})";

    TEST(GdalUtilSandbox, strictModeRefusesToStartWithoutLandlockAndNamesTheLayer)
    {
        const Captured run = runWithoutLandlock({"--serve"}, {{kSandboxEnv, "strict"}}, std::string(kProbeLine) + "\n");
        EXPECT_FALSE(run.timedOut);
        EXPECT_NE(run.exitCode, 0);
        EXPECT_NE(run.exitCode, -1) << "the program must exit, not be killed";
        EXPECT_TRUE(run.out.empty()) << "a server that refused to start answers nothing: " << run.out;
        EXPECT_NE(run.err.find("landlock"), std::string::npos) << run.err;
        EXPECT_NE(run.err.find("strict"), std::string::npos) << run.err;
    }

    TEST(GdalUtilSandbox, bestEffortModeWarnsOnceAboutTheMissingLayerAndServesEveryRequest)
    {
        // Three requests, three workers: one warning, from the server, when it starts
        const std::string input = std::string(kProbeLine) + "\n" + kProbeLine + "\n" + kProbeLine + "\n";
        const Captured run = runWithoutLandlock({"--serve"}, {{kSandboxEnv, "best-effort"}}, input);
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, 0) << run.err;
        EXPECT_EQ(countOf(run.out, "\"error\":\"unreadable\""), 3u) << "the requests are served: " << run.out;
        EXPECT_EQ(countOf(run.err, "Sandbox layers not available"), 1u) << run.err;
        EXPECT_NE(run.err.find("landlock"), std::string::npos) << run.err;
    }

    TEST(GdalUtilSandbox, aOneShotProcessWarnsOnceToo)
    {
        const std::string line = std::string(R"({"id":1,"op":"probe","path":"/nonexistent/none.tif","params":{"kind":"raster"}})");
        const Captured run = runWithoutLandlock({"--request", line}, {{kSandboxEnv, "best-effort"}}, "");
        EXPECT_EQ(countOf(run.err, "Sandbox layers not available"), 1u) << run.err;
        EXPECT_NE(run.err.find("landlock"), std::string::npos) << run.err;
        EXPECT_EQ(countOf(run.out, "\"error\":\"unreadable\""), 1u) << run.out;
    }

    TEST(GdalUtilSandbox, aOneShotProcessInStrictModeAnswersWithTheRefusalAndRunsNothing)
    {
        const std::string line = std::string(R"({"id":1,"op":"probe","path":"/nonexistent/none.tif","params":{"kind":"raster"}})");
        const Captured run = runWithoutLandlock({"--request", line}, {{kSandboxEnv, "strict"}}, "");
        EXPECT_EQ(run.exitCode, 1) << run.err;
        const auto response = raster::decodeResponse(run.out);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Internal);
        EXPECT_NE(response.message.find("landlock"), std::string::npos) << response.message;
    }

    TEST(GdalUtilSandbox, offModeAppliesNoLayerAndHasNothingToWarnAbout)
    {
        const Captured run = runWithoutLandlock({"--serve"}, {{kSandboxEnv, "off"}}, std::string(kProbeLine) + "\n");
        EXPECT_EQ(run.exitCode, 0) << run.err;
        EXPECT_EQ(countOf(run.out, "\"error\":\"unreadable\""), 1u) << run.out;
        EXPECT_EQ(countOf(run.err, "Sandbox layers not available"), 0u) << run.err;
        EXPECT_NE(run.err.find("off"), std::string::npos) << "the server says once that it is not confined: " << run.err;
    }

    TEST(GdalUtilSandbox, theReportOfAHostWithoutLandlockShowsItMissingAndStillSucceeds)
    {
        const Captured run = runWithoutLandlock({"--sandbox-report"}, {{kSandboxEnv, "strict"}}, "");
        EXPECT_EQ(run.exitCode, 0) << run.err;
        EXPECT_NE(run.out.find("landlock: missing"), std::string::npos) << run.out;
        EXPECT_NE(run.out.find("rlimits: active"), std::string::npos) << run.out;
    }
#endif

    // ------------------------------------------------------------------ what a worker may read

#ifdef __linux__
    class GdalUtilSandboxFiles : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ASSERT_FALSE(tool().empty()) << "gdal-util is built next to the test binary";
            area_ = std::make_unique<TestArea>(std::string("GdalUtilSandbox.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            folderA_ = area_->getFolder("a");
            folderB_ = area_->getFolder("b");
            testutils::GeoRasterSpec spec;
            spec.width = 64;
            spec.height = 48;
            spec.bands = 3;
            spec.epsg = 32632;
            spec.originX = 500000.0;
            spec.originY = 5000000.0;
            spec.pixelSize = 0.5;
            spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y) % 256); };
            raster_ = testutils::createGeoRaster(folderA_ / "ortho.tif", spec);
            elsewhere_ = testutils::createGeoRaster(folderB_ / "secret.tif", spec);
        }

        std::unique_ptr<TestArea> area_;
        fs::path folderA_;
        fs::path folderB_;
        fs::path raster_;
        fs::path elsewhere_;
    };

    // A DXF with one point at UTM coordinates of zone 32N, and the .prj that gives it its system
    fs::path writeDrawing(const fs::path &dxf, bool withPrj)
    {
        writeFile(dxf,
                  "0\nSECTION\n2\nHEADER\n0\nENDSEC\n"
                  "0\nSECTION\n2\nENTITIES\n0\nPOINT\n8\n0\n10\n500000.0\n20\n5000000.0\n30\n0.0\n0\nENDSEC\n0\nEOF\n");
        if (withPrj)
        {
            fs::path prj = dxf;
            prj.replace_extension(".prj");
            writeFile(prj, cad::prjContent("EPSG:32632"));
        }
        return dxf;
    }

    TEST_F(GdalUtilSandboxFiles, strictModeServesTheOperationsOfTheHost)
    {
        if (!hostOffers("landlock") || !hostOffers("seccomp"))
            GTEST_SKIP() << "strict mode needs every layer: " << (hostOffers("landlock") ? kNeedsSeccomp : kNeedsLandlock);

        Server server({{kSandboxEnv, "strict"}});

        const auto probe = server.ask(probeRequest(1, raster_));
        ASSERT_TRUE(probe.ok) << probe.message;
        EXPECT_EQ(raster::probeResultFromJson(probe.result).type, "GeoRaster");

        // The output goes to the folder of the request, and only the folder of the request is writable
        const fs::path outputFolder = area_->getFolder("out");
        const auto thumbnail = server.ask(request(2, raster::op::kThumbnail, raster_, outputFolder / "t.webp", json{{"size", 32}}));
        ASSERT_TRUE(thumbnail.ok) << thumbnail.message;
        EXPECT_TRUE(fs::exists(outputFolder / "t.webp"));

        const auto normalize = server.ask(request(3, raster::op::kNormalize, raster_, outputFolder / "n.tif"));
        ASSERT_TRUE(normalize.ok) << normalize.message;
        EXPECT_TRUE(fs::exists(outputFolder / "n.tif"));

        // A vector, and a drawing converted into the folder of the request from a source and a .prj
        // that lie in another folder: the folders the request names are readable too
        const fs::path source = writeDrawing(folderB_ / "site.dxf", true);
        fs::path converted = folderA_ / "converted.dxf";
        fs::copy_file(source, converted);
        raster::ProbeParams params = vectorParams();
        params.converted = true;
        params.source = source;
        const auto drawing = server.ask(probeRequest(4, converted, params));
        ASSERT_TRUE(drawing.ok) << drawing.message;
        EXPECT_TRUE(raster::probeResultFromJson(drawing.result).properties.at("georeferenced").get<bool>());

        // The server is still the one that started: nothing killed it
        EXPECT_TRUE(server.ask(probeRequest(5, raster_)).ok);
    }

    // A link in the folder of the request that points into another folder: the open policy of the
    // program has no quarrel with it, the file system does
    TEST_F(GdalUtilSandboxFiles, aWorkerCannotReadAFileOutsideTheFoldersOfItsRequest)
    {
        if (!hostOffers("landlock"))
            GTEST_SKIP() << kNeedsLandlock;

        const fs::path link = folderA_ / "link.tif";
        fs::create_symlink(elsewhere_, link);

        {
            Server server({{kSandboxEnv, "best-effort"}});
            const auto denied = server.ask(probeRequest(1, link));
            EXPECT_FALSE(denied.ok);
            EXPECT_EQ(denied.error, raster::ErrorCode::Unreadable);
            // The folder of the request is readable
            EXPECT_TRUE(server.ask(probeRequest(2, raster_)).ok);
        }
        {
            // Without the sandbox the same request reads the file: the test can fail
            Server server({{kSandboxEnv, "off"}});
            const auto allowed = server.ask(probeRequest(3, link));
            EXPECT_TRUE(allowed.ok) << allowed.message;
        }
    }

    // A listener on the loopback interface that closes every connection it accepts, so a client
    // that connects fails at once instead of waiting for an answer
    class Listener
    {
      public:
        Listener()
        {
            fd_ = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t length = sizeof(address);
            EXPECT_EQ(bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
            EXPECT_EQ(listen(fd_, 16), 0);
            EXPECT_EQ(getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length), 0);
            port_ = ntohs(address.sin_port);
            acceptor_ = std::thread([this] {
                for (;;)
                {
                    const int connection = accept(fd_, nullptr, nullptr);
                    if (connection < 0)
                        return;
                    connections_++;
                    close(connection);
                }
            });
        }
        ~Listener()
        {
            stop();
            close(fd_);
        }

        int port() const { return port_; }

        // Whether anything connected: stops the listener, then counts what the thread accepted and
        // what still waits to be accepted
        bool receivedAConnection()
        {
            stop();
            for (;;)
            {
                const int connection = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK);
                if (connection < 0)
                    break;
                connections_++;
                close(connection);
            }
            return connections_ > 0;
        }

      private:
        void stop()
        {
            if (acceptor_.joinable())
            {
                shutdown(fd_, SHUT_RDWR);
                acceptor_.join();
            }
        }

        int fd_ = -1;
        int port_ = 0;
        std::atomic<int> connections_{0};
        std::thread acceptor_;
    };

    // The program never asks for a URL (its client sends paths of files), but a file can name one,
    // and a request that is a URL stands for any of the ways a parser can be led to the network:
    // the filter refuses the socket, GDAL reports an error, and the request ends like any other
    TEST_F(GdalUtilSandboxFiles, aWorkerCannotReachTheNetworkAndTheRequestEndsWithAnError)
    {
        if (!hostOffers("seccomp"))
            GTEST_SKIP() << kNeedsSeccomp;

        const auto probeUrl = [](Server &server, int port) {
            return server.ask(probeRequest(1, "/vsicurl/http://127.0.0.1:" + std::to_string(port) + "/a.tif"));
        };
        {
            Listener listener;
            Server server({{kSandboxEnv, "best-effort"}});
            const auto response = probeUrl(server, listener.port());
            EXPECT_FALSE(response.ok);
            EXPECT_EQ(response.error, raster::ErrorCode::Unreadable) << response.message;
            EXPECT_FALSE(listener.receivedAConnection());
        }
        {
            // Without the sandbox the same request connects: the test can fail
            Listener listener;
            Server server({{kSandboxEnv, "off"}});
            const auto response = probeUrl(server, listener.port());
            EXPECT_FALSE(response.ok);
            EXPECT_TRUE(listener.receivedAConnection());
        }
    }

    // ------------------------------------------------------------------ what a worker is limited to

    // The value of a line of /proc/<pid>/status: "Seccomp:\t2" is "2"
    std::string statusField(unsigned long pid, const std::string &name)
    {
        std::ifstream in("/proc/" + std::to_string(pid) + "/status");
        std::string line;
        while (std::getline(in, line))
            if (line.rfind(name + ":", 0) == 0)
            {
                const size_t start = line.find_first_not_of(" \t", name.size() + 1);
                return start == std::string::npos ? std::string() : line.substr(start);
            }
        return {};
    }

    struct Limit
    {
        std::string soft;
        std::string hard;
        uint64_t softValue() const { return soft == "unlimited" ? UINT64_MAX : std::stoull(soft); }
    };

    // The soft and hard value of a row of /proc/<pid>/limits ("Max cpu time")
    Limit limitOf(unsigned long pid, const std::string &name)
    {
        std::ifstream in("/proc/" + std::to_string(pid) + "/limits");
        std::string line;
        while (std::getline(in, line))
            if (line.rfind(name, 0) == 0)
            {
                std::istringstream fields(line.substr(name.size()));
                Limit limit;
                fields >> limit.soft >> limit.hard;
                return limit;
            }
        ADD_FAILURE() << "no limit named " << name;
        return {};
    }

    std::string firstLineOf(const fs::path &file)
    {
        std::ifstream in(file);
        std::string line;
        std::getline(in, line);
        return line;
    }

    // The parent of a process, 0 when it cannot be read
    unsigned long parentOf(const fs::path &procEntry)
    {
        std::ifstream stat(procEntry / "stat");
        std::string line;
        if (!std::getline(stat, line))
            return 0;
        // pid (name) state ppid ...: the name may hold spaces and brackets, so read after the last )
        const size_t close = line.rfind(')');
        if (close == std::string::npos)
            return 0;
        std::istringstream rest(line.substr(close + 1));
        char state = 0;
        unsigned long ppid = 0;
        rest >> state >> ppid;
        return ppid;
    }

    unsigned long parentOf(unsigned long pid)
    {
        return parentOf(fs::path("/proc") / std::to_string(pid));
    }

    // The processes whose parent is `parent`
    std::vector<unsigned long> childrenOf(unsigned long parent)
    {
        std::vector<unsigned long> children;
        for (const auto &entry : fs::directory_iterator("/proc"))
        {
            const std::string name = entry.path().filename().string();
            if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            if (parentOf(entry.path()) == parent)
                children.push_back(std::stoul(name));
        }
        return children;
    }

    // What each open descriptor of a process is: "pipe:[inode]", "socket:[inode]", a path. The two
    // ends of a pipe have the same text.
    std::map<int, std::string> descriptorsOf(unsigned long pid)
    {
        std::map<int, std::string> links;
        for (const auto &entry : fs::directory_iterator(fs::path("/proc") / std::to_string(pid) / "fd"))
        {
            std::error_code error;
            const fs::path target = fs::read_symlink(entry.path(), error);
            if (!error)
                links[std::stoi(entry.path().filename().string())] = target.string();
        }
        return links;
    }

    bool isChannel(const std::string &link)
    {
        return link.rfind("pipe:", 0) == 0 || link.rfind("socket:", 0) == 0;
    }

    // A named pipe in place of the input. Whatever reads it waits for the test: the process that
    // opens it is held there, after it has confined itself and before it has read anything, for
    // as long as the test keeps the writing end open. Opening the writing end returns when the
    // process has opened the reading end, which tells the test the process is there.
    class HeldInput
    {
      public:
        explicit HeldInput(const fs::path &fifo) : fifo_(fifo)
        {
            EXPECT_EQ(mkfifo(fifo.c_str(), 0600), 0);
        }
        ~HeldInput() { release(); }

        // Blocks until a reader has opened the pipe; false when none did in time
        bool waitForReader()
        {
            auto opened = std::async(std::launch::async, [this] { return open(fifo_.c_str(), O_WRONLY); });
            if (opened.wait_for(kPatience) != std::future_status::ready)
            {
                // Let the thread go: a reader of our own completes its open
                const int reader = open(fifo_.c_str(), O_RDONLY | O_NONBLOCK);
                opened.wait();
                if (reader >= 0)
                    close(reader);
                const int writer = opened.get();
                if (writer >= 0)
                    close(writer);
                return false;
            }
            writer_ = opened.get();
            return writer_ >= 0;
        }

        // Lets the reader see the end of the file
        void release()
        {
            if (writer_ >= 0)
                close(writer_);
            writer_ = -1;
        }

      private:
        fs::path fifo_;
        int writer_ = -1;
    };

    struct ExpectedLimits
    {
        const char *op;
        uint64_t cpuSeconds;
        uint64_t fileBytes;
        uint64_t openFiles;
        uint64_t minMemoryBytes;
    };

    constexpr uint64_t kMiB = 1024ULL * 1024;
    constexpr uint64_t kGiB = 1024 * kMiB;

    // What a worker has done to itself, read while it waits on the input
    void expectConfined(unsigned long worker, const ExpectedLimits &expected)
    {
        SCOPED_TRACE(expected.op);
        EXPECT_EQ(statusField(worker, "NoNewPrivs"), "1");
        EXPECT_EQ(statusField(worker, "Seccomp"), "2");
        EXPECT_EQ(firstLineOf("/proc/" + std::to_string(worker) + "/oom_score_adj"), "1000");

        const Limit cpu = limitOf(worker, "Max cpu time");
        EXPECT_EQ(cpu.softValue(), expected.cpuSeconds);
        // The hard limit lies one second beyond, so that the soft one ends the process with SIGXCPU
        EXPECT_EQ(cpu.hard, std::to_string(expected.cpuSeconds + 1));
        EXPECT_EQ(limitOf(worker, "Max file size").softValue(), expected.fileBytes);
        EXPECT_EQ(limitOf(worker, "Max open files").softValue(), expected.openFiles);
        EXPECT_EQ(limitOf(worker, "Max core file size").softValue(), 0u);
        EXPECT_GE(limitOf(worker, "Max address space").softValue(), expected.minMemoryBytes);
        EXPECT_NE(limitOf(worker, "Max address space").soft, "unlimited");
    }

    void expectNotConfined(unsigned long worker)
    {
        EXPECT_EQ(statusField(worker, "NoNewPrivs"), "0");
        EXPECT_EQ(statusField(worker, "Seccomp"), "0");
        // The score of the test's own session, not the 1000 of a confined worker
        EXPECT_NE(firstLineOf("/proc/" + std::to_string(worker) + "/oom_score_adj"), "1000");
        EXPECT_EQ(limitOf(worker, "Max address space").soft, "unlimited");
        EXPECT_EQ(limitOf(worker, "Max cpu time").soft, "unlimited");
    }

    json paramsFor(const char *op)
    {
        if (std::string(op) == raster::op::kThumbnail)
            return json{{"size", 32}};
        if (std::string(op) == raster::op::kGeoproject)
            return json{{"width", 100}, {"height", 80}, {"footprint", json::array({json::array({9.1, 45.3}), json::array({9.1, 45.2}),
                                                                                   json::array({9.2, 45.2}), json::array({9.2, 45.3})})}};
        if (std::string(op) == raster::op::kProbe)
            return json{{"kind", "raster"}};
        return json::object();
    }

    // Starts a server, sends the request for `op` on a named pipe, waits for the worker to open
    // it and hands the worker's pid to `inspect`
    template <typename Inspect> void inspectWorker(const fs::path &folder, const char *op, const Environment &environment, Inspect inspect)
    {
        HeldInput held(folder / "held.tif");
        const fs::path outputFolder = folder / "out";
        fs::create_directories(outputFolder);

        Server server(environment);
        server.send(request(1, op, folder / "held.tif", std::string(op) == raster::op::kProbe ? fs::path() : outputFolder / "result",
                            paramsFor(op)));
        ASSERT_TRUE(held.waitForReader()) << "no worker opened the input: " << server.session().stderrTail();

        const auto workers = childrenOf(server.session().pid());
        ASSERT_EQ(workers.size(), 1u);
        inspect(workers.front());

        // The worker reads an empty file and fails; the server answers and goes on
        held.release();
        const auto response = server.receive();
        EXPECT_FALSE(response.ok);
        EXPECT_NE(response.error, raster::ErrorCode::Crashed) << response.message;
    }

    TEST_F(GdalUtilSandboxFiles, eachOperationRunsInAWorkerThatHasConfinedItselfWithItsOwnLimits)
    {
        if (!hostOffers("landlock") || !hostOffers("seccomp"))
            GTEST_SKIP() << (hostOffers("landlock") ? kNeedsSeccomp : kNeedsLandlock);

        // The cpu time ends before the 30 s and 120 s the client waits; a thumbnail is a file the
        // client accepts up to 64 MiB; the address space is a share of the memory of the machine
        // with a floor
        const std::vector<ExpectedLimits> table = {
            {raster::op::kProbe, 25, 16 * kMiB, 256, 2 * kGiB},
            {raster::op::kThumbnail, 25, 64 * kMiB, 256, 2 * kGiB},
            {raster::op::kNormalize, 110, 128 * kGiB, 1024, 4 * kGiB},
            {raster::op::kGeoproject, 110, 16 * kGiB, 1024, 4 * kGiB},
        };
        for (const auto &expected : table)
        {
            const fs::path folder = area_->getFolder(std::string("held-") + expected.op);
            inspectWorker(folder, expected.op, {{kSandboxEnv, "strict"}},
                          [&](unsigned long worker) { expectConfined(worker, expected); });
        }
    }

    // Only the server answers the host. A worker that a file took over must find no descriptor that
    // leads to the host but standard error, which the host only logs: not the copy of standard
    // output the server writes its responses to (a line written there would be read as the answer
    // to a request), not the requests on standard input
    TEST_F(GdalUtilSandboxFiles, aWorkerKeepsNoDescriptorThatLeadsToTheHostButStandardError)
    {
        const fs::path folder = area_->getFolder("held-descriptors");
        inspectWorker(folder, raster::op::kProbe, {}, [](unsigned long worker) {
            const auto host = descriptorsOf(static_cast<unsigned long>(getpid()));
            const auto server = descriptorsOf(parentOf(worker));
            const auto kept = descriptorsOf(worker);
            ASSERT_EQ(kept.count(STDERR_FILENO), 1u);
            ASSERT_EQ(server.count(STDIN_FILENO), 1u);
            const std::string standardError = kept.at(STDERR_FILENO);
            EXPECT_EQ(kept.count(STDIN_FILENO) == 1 ? kept.at(STDIN_FILENO) : std::string(), "/dev/null");

            for (const auto &[fd, link] : kept)
            {
                if (!isChannel(link) || link == standardError)
                    continue;
                EXPECT_NE(link, server.at(STDIN_FILENO)) << "descriptor " << fd << " of the worker reads the requests";
                for (const auto &[hostFd, hostLink] : host)
                    EXPECT_NE(link, hostLink) << "descriptor " << fd << " of the worker leads to descriptor " << hostFd
                                              << " of the host";
            }
        });
    }

    TEST_F(GdalUtilSandboxFiles, withTheSandboxOffTheWorkerIsNotConfined)
    {
        const fs::path folder = area_->getFolder("held-off");
        inspectWorker(folder, raster::op::kNormalize, {{kSandboxEnv, "off"}},
                      [](unsigned long worker) { expectNotConfined(worker); });
    }

    // A build runs in a process of its own that confines itself: its cpu time is the run time the
    // client allows, for each of the threads it may use
    TEST_F(GdalUtilSandboxFiles, aBuildProcessConfinesItselfWithTheRunTimeOfTheClient)
    {
        if (!hostOffers("landlock") || !hostOffers("seccomp"))
            GTEST_SKIP() << (hostOffers("landlock") ? kNeedsSeccomp : kNeedsLandlock);

        HeldInput held(area_->getFolder("held-build") / "held.tif");
        const fs::path outputFolder = area_->getFolder("held-build/out");

        Session session;
        process::SessionOptions options;
        options.envOverrides = {{kSandboxEnv, "strict"}, {raster::kGdalUtilMaxSecondsEnv, "1000"}};
        const raster::Request build = request(1, raster::op::kCog, area_->getFolder("held-build") / "held.tif",
                                              outputFolder / "cog.tif", json{{"threads", 1}});
        ASSERT_TRUE(session.start(tool(), {"--request", raster::encodeRequest(build)}, options)) << session.startError();
        ASSERT_TRUE(held.waitForReader()) << session.stderrTail();

        const unsigned long pid = session.pid();
        EXPECT_EQ(statusField(pid, "Seccomp"), "2");
        EXPECT_EQ(firstLineOf("/proc/" + std::to_string(pid) + "/oom_score_adj"), "1000");
        const uint64_t threads = std::max(1u, std::thread::hardware_concurrency());
        EXPECT_EQ(limitOf(pid, "Max cpu time").softValue(), 1000 * threads);
        EXPECT_EQ(limitOf(pid, "Max file size").softValue(), 1024 * kGiB);
        EXPECT_EQ(limitOf(pid, "Max open files").softValue(), 4096u);
        EXPECT_GE(limitOf(pid, "Max address space").softValue(), 4 * kGiB);

        // The build opens its input more than once: it is not worth waiting for its failure
        session.kill();
    }

    // ------------------------------------------------------------------ the limits end in `limit`

    // A raster whose blocks are large and cost a lot of cpu time: three bands of 12288 x 12288
    // floats, each band one compressed block of 576 MiB (zeros, so the file is a few hundred KiB).
    // Reading it needs more than 512 MiB at once, and copying it takes seconds of cpu time.
    class GdalUtilSandboxLimits : public ::testing::Test
    {
      protected:
        static void SetUpTestSuite()
        {
            area_ = new TestArea("GdalUtilSandbox.heavy", true);
            heavy_ = new fs::path(area_->getFolder() / "heavy.tif");

            constexpr int kSide = 12288;
            const char *options[] = {"COMPRESS=DEFLATE", "TILED=NO", "BLOCKYSIZE=12288", "BIGTIFF=IF_SAFER", nullptr};
            GDALDatasetH dataset = GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(*heavy_).c_str(), kSide, kSide, 3,
                                              GDT_Float32, const_cast<char **>(options));
            ASSERT_NE(dataset, nullptr);
            const double transform[6] = {500000.0, 1.0, 0.0, 5000000.0, 0.0, -1.0};
            GDALSetGeoTransform(dataset, transform);
            OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
            OSRImportFromEPSG(srs, 32632);
            char *wkt = nullptr;
            OSRExportToWkt(srs, &wkt);
            GDALSetProjection(dataset, wkt);
            CPLFree(wkt);
            OSRDestroySpatialReference(srs);
            for (int band = 1; band <= 3; band++)
                ASSERT_EQ(GDALFillRaster(GDALGetRasterBand(dataset, band), 0.0, 0.0), CE_None);
            GDALClose(dataset);
        }

        static void TearDownTestSuite()
        {
            delete heavy_;
            delete area_;
            heavy_ = nullptr;
            area_ = nullptr;
        }

        static fs::path outputFolder(const char *name) { return area_->getFolder(name); }

        static TestArea *area_;
        static fs::path *heavy_;
    };

    TestArea *GdalUtilSandboxLimits::area_ = nullptr;
    fs::path *GdalUtilSandboxLimits::heavy_ = nullptr;

    TEST_F(GdalUtilSandboxLimits, aWorkerThatCannotAllocateWhatTheFileNeedsEndsWithLimitAndTheServerGoesOn)
    {
        // The server and its workers have 512 MiB of address space; a block of the file is 576 MiB
        Server server({{kSandboxEnv, "best-effort"}}, 512 * kMiB);
        const auto response = server.ask(request(1, raster::op::kNormalize, *heavy_, outputFolder("memory") / "n.tif"));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Limit) << response.message;

        // A file that fits is served by the next worker
        const fs::path small = outputFolder("memory-small") / "small.tif";
        testutils::GeoRasterSpec spec;
        spec.width = 32;
        spec.height = 32;
        spec.bands = 1;
        spec.epsg = 32632;
        testutils::createGeoRaster(small, spec);
        EXPECT_TRUE(server.ask(probeRequest(2, small)).ok);
    }

    TEST_F(GdalUtilSandboxLimits, aWorkerThatUsesMoreCpuTimeThanItMayEndsWithLimitAndTheServerGoesOn)
    {
        // The server has 2 s of cpu time to its name (the system's limit, set by the shell), so the
        // workers have the second below it: the copy of the file takes several
        Server server("ulimit -t 2", {{kSandboxEnv, "best-effort"}});
        const auto response = server.ask(request(1, raster::op::kNormalize, *heavy_, outputFolder("cpu") / "n.tif"));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Limit) << response.message;
        EXPECT_NE(response.message.find("cpu time"), std::string::npos) << response.message;

        const fs::path small = outputFolder("cpu-small") / "small.tif";
        testutils::GeoRasterSpec spec;
        spec.width = 32;
        spec.height = 32;
        spec.bands = 1;
        spec.epsg = 32632;
        testutils::createGeoRaster(small, spec);
        EXPECT_TRUE(server.ask(probeRequest(2, small)).ok);
    }

    // The process of a build confines itself, so its end by the limit is not a response: the
    // client reads it from the signal
    class ShellLimitedRunner : public process::IProcessRunner
    {
      public:
        process::Result run(const fs::path &binary, const std::vector<std::string> &args,
                            const process::Options &options) const override
        {
            std::vector<std::string> shellArgs = {"-c", "ulimit -t 2; exec \"$0\" \"$@\"", io::toUtf8(binary)};
            shellArgs.insert(shellArgs.end(), args.begin(), args.end());
            return process::systemProcessRunner().run("/bin/sh", shellArgs, options);
        }
    };

    TEST_F(GdalUtilSandboxLimits, aBuildThatUsesMoreCpuTimeThanItMayIsALimitForTheHost)
    {
        const ShellLimitedRunner runner;
        raster::GdalUtilHelperOptions options;
        options.locate = [] { return tool(); };
        options.runner = &runner;
        raster::GdalUtilHelper helper(options);

        const fs::path folder = outputFolder("build");
        try
        {
            helper.buildCog(*heavy_, folder / "cog.tif", json::object(), raster::ProgressCallback());
            FAIL() << "the build finished";
        }
        catch (const raster::RasterNotReadException &e)
        {
            EXPECT_EQ(e.code(), raster::ErrorCode::Limit) << e.what();
        }
        helper.shutdown();
    }
#endif
} // namespace
