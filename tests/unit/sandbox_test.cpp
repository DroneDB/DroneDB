/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The sandbox library (vendor/sandbox) confines the process that applies it, for good, so every
// test applies it in a forked child and the child reports back through a pipe. A layer the kernel
// does not offer skips the tests that need it, with the reason. Each test that shows a layer
// refusing something runs the same code with Mode::Off too, to show that the test can fail: the
// connection reaches the listener, the file opens, the loop goes on.

#include <algorithm>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "sandbox/sandbox.h"

namespace
{
    using sandbox::Mode;

    TEST(Sandbox, parseModeKnowsTheThreeModes)
    {
        std::string warning = "stale";
        EXPECT_EQ(sandbox::parseMode("strict", &warning), Mode::Strict);
        EXPECT_TRUE(warning.empty());
        EXPECT_EQ(sandbox::parseMode("best-effort", &warning), Mode::BestEffort);
        EXPECT_EQ(sandbox::parseMode("off", &warning), Mode::Off);
        EXPECT_EQ(sandbox::parseMode("  Strict\n", &warning), Mode::Strict);
        EXPECT_EQ(sandbox::parseMode("OFF", &warning), Mode::Off);
        EXPECT_TRUE(warning.empty());
        EXPECT_STREQ(sandbox::modeName(Mode::Strict), "strict");
        EXPECT_STREQ(sandbox::modeName(Mode::BestEffort), "best-effort");
        EXPECT_STREQ(sandbox::modeName(Mode::Off), "off");
    }

    TEST(Sandbox, parseModeDefaultsToBestEffortAndWarnsOnlyForAnUnknownValue)
    {
        std::string warning;
        EXPECT_EQ(sandbox::parseMode(nullptr, &warning), Mode::BestEffort);
        EXPECT_TRUE(warning.empty());
        EXPECT_EQ(sandbox::parseMode("", &warning), Mode::BestEffort);
        EXPECT_TRUE(warning.empty());
        EXPECT_EQ(sandbox::parseMode("   ", &warning), Mode::BestEffort);
        EXPECT_TRUE(warning.empty());

        EXPECT_EQ(sandbox::parseMode("paranoid", &warning), Mode::BestEffort);
        EXPECT_NE(warning.find("paranoid"), std::string::npos) << warning;
        EXPECT_NE(warning.find("best-effort"), std::string::npos) << warning;

        // The warning is optional
        EXPECT_EQ(sandbox::parseMode("paranoid"), Mode::BestEffort);
    }

    TEST(Sandbox, offAppliesNothingAndReportsNoLayer)
    {
        const sandbox::Report report = sandbox::apply(sandbox::Policy(), Mode::Off);
        EXPECT_TRUE(report.ok);
        EXPECT_TRUE(report.layers.empty());
        EXPECT_TRUE(report.warning().empty());
    }

    TEST(Sandbox, probeListsEveryLayerOnePerLine)
    {
        const sandbox::Report report = sandbox::probe();
        ASSERT_FALSE(report.layers.empty());
        const std::string text = report.describe();
        for (const auto &layer : report.layers)
            EXPECT_NE(text.find(layer.name + ": " + (layer.active ? "active" : "missing")), std::string::npos)
                << layer.name << " in " << text;
        size_t lines = 0;
        for (char c : text)
            lines += c == '\n' ? 1 : 0;
        EXPECT_EQ(lines, report.layers.size()) << text;
#ifdef __linux__
        std::vector<std::string> names;
        for (const auto &layer : report.layers)
            names.push_back(layer.name);
        for (const char *expected : {"rlimits", "seccomp", "landlock"})
            EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end()) << expected;
#endif
    }
} // namespace

#ifdef __linux__

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

#include <ogr_api.h>
#include <proj.h>

#include "gdal_inc.h"
#include "gdal_raii.h"
#include "mio.h"
#include "raster_test_utils.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;
    using sandbox::Mode;

    // Syscalls newer than the oldest kernel headers the tests build against
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif

    // ---- Running a part of a test in a child ------------------------------------------------

    struct ChildResult
    {
        int status = 0;
        std::map<std::string, std::string> fields;

        bool exited() const
        {
            return WIFEXITED(status);
        }
        bool killedBy(int signal) const
        {
            return WIFSIGNALED(status) && WTERMSIG(status) == signal;
        }
        std::string field(const std::string &key) const
        {
            const auto it = fields.find(key);
            return it == fields.end() ? std::string("<missing>") : it->second;
        }
    };

    // What the child tells the parent: lines "key=value"
    class Fields
    {
      public:
        template <typename T> Fields &set(const std::string &key, const T &value)
        {
            text_ << key << "=" << value << "\n";
            return *this;
        }
        std::string str() const
        {
            return text_.str();
        }

      private:
        std::ostringstream text_;
    };

    // Runs `body` in a forked child, which writes what it returns to a pipe and exits. The wait
    // for the child has a deadline: a child that hangs fails the test, it does not hang it.
    ChildResult inChild(const std::function<std::string()> &body)
    {
        int channel[2];
        if (pipe(channel) != 0)
            throw std::runtime_error("pipe failed");
        const pid_t child = fork();
        if (child < 0)
            throw std::runtime_error("fork failed");
        if (child == 0)
        {
            close(channel[0]);
            const std::string text = body();
            const char *data = text.data();
            size_t left = text.size();
            while (left > 0)
            {
                const ssize_t written = write(channel[1], data, left);
                if (written <= 0)
                    break;
                data += written;
                left -= static_cast<size_t>(written);
            }
            _exit(0);
        }
        close(channel[1]);

        std::string text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        char buffer[4096];
        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd waiting{channel[0], POLLIN, 0};
            if (left.count() <= 0 || poll(&waiting, 1, static_cast<int>(left.count())) <= 0)
            {
                kill(child, SIGKILL);
                break;
            }
            const ssize_t received = read(channel[0], buffer, sizeof(buffer));
            if (received <= 0)
                break;
            text.append(buffer, static_cast<size_t>(received));
        }
        close(channel[0]);

        ChildResult result;
        waitpid(child, &result.status, 0);
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            const size_t equals = line.find('=');
            if (equals != std::string::npos)
                result.fields[line.substr(0, equals)] = line.substr(equals + 1);
        }
        return result;
    }

    bool layerActive(const char *name)
    {
        for (const auto &layer : sandbox::probe().layers)
            if (layer.name == name)
                return layer.active;
        return false;
    }

    std::string layerDetail(const char *name)
    {
        for (const auto &layer : sandbox::probe().layers)
            if (layer.name == name)
                return layer.detail;
        return std::string();
    }

    // Skips the test when the kernel lacks one of the layers it needs
#define SKIP_WITHOUT_LAYER(name)                                                                              \
    if (!layerActive(name))                                                                                   \
        GTEST_SKIP() << "The kernel offers no " << name << " layer: " << layerDetail(name)

    // Applies the policy in the child, as a worker does at its start, and leaves the child when
    // the sandbox refuses
    std::string applyOrExplain(const sandbox::Policy &policy, Mode mode)
    {
        const sandbox::Report report = sandbox::apply(policy, mode);
        return report.ok ? std::string() : report.error;
    }

    // ---- A port on the loopback that records whether anyone connected ------------------------

    class Listener
    {
      public:
        Listener() : fd_(socket(AF_INET, SOCK_STREAM, 0))
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t length = sizeof(address);
            if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr *>(&address), length) != 0 || listen(fd_, 4) != 0 ||
                getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
                throw std::runtime_error("cannot listen on the loopback");
            port_ = ntohs(address.sin_port);
        }
        ~Listener()
        {
            if (fd_ >= 0)
                close(fd_);
        }
        Listener(const Listener &) = delete;
        Listener &operator=(const Listener &) = delete;

        int port() const
        {
            return port_;
        }
        // Whether a connection is waiting. The tests ask after the child that connects has
        // ended: a loopback connection is complete before connect() returns, so there is
        // nothing to wait for.
        bool connected() const
        {
            pollfd waiting{fd_, POLLIN, 0};
            return poll(&waiting, 1, 0) > 0;
        }

      private:
        int fd_;
        int port_ = 0;
    };

    // ---- Network ------------------------------------------------------------------------------

    // What a worker's attempt to reach `port` on the loopback comes to
    std::string connectToLoopback(int port, Mode mode)
    {
        Fields out;
        const std::string refused = applyOrExplain(sandbox::Policy(), mode);
        out.set("applied", refused.empty() ? "yes" : refused);

        const int inet = socket(AF_INET, SOCK_STREAM, 0);
        out.set("inet", inet < 0 ? errno : 0);
        if (inet >= 0)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<uint16_t>(port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            out.set("connect", connect(inet, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 ? 0 : errno);
            close(inet);
        }
        const int inet6 = socket(AF_INET6, SOCK_STREAM, 0);
        out.set("inet6", inet6 < 0 ? errno : 0);
        if (inet6 >= 0)
            close(inet6);
        const int udp = socket(AF_INET, SOCK_DGRAM, 0);
        out.set("udp", udp < 0 ? errno : 0);
        if (udp >= 0)
            close(udp);
        const int netlink = socket(AF_NETLINK, SOCK_RAW, 0);
        out.set("netlink", netlink < 0 ? errno : 0);
        if (netlink >= 0)
            close(netlink);
        int pair[2];
        out.set("inetpair", socketpair(AF_INET, SOCK_STREAM, 0, pair) == 0 ? 0 : errno);
        out.set("unixpair", socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 ? 0 : errno);
        return out.str();
    }

    TEST(Sandbox, networkSocketsAreRefusedAndTheListenerSeesNoConnection)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        Listener listener;
        const ChildResult result = inChild([&] { return connectToLoopback(listener.port(), Mode::BestEffort); });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("applied"), "yes");
        EXPECT_EQ(result.field("inet"), std::to_string(EPERM));
        EXPECT_EQ(result.field("inet6"), std::to_string(EPERM));
        EXPECT_EQ(result.field("udp"), std::to_string(EPERM));
        EXPECT_EQ(result.field("netlink"), std::to_string(EPERM));
        EXPECT_EQ(result.field("inetpair"), std::to_string(EPERM));
        // The local sockets of the C library stay
        EXPECT_EQ(result.field("unixpair"), "0");
        EXPECT_FALSE(listener.connected());
    }

    // The same code without the sandbox connects: what the test above checks can fail
    TEST(Sandbox, withTheSandboxOffTheSameCodeReachesTheListener)
    {
        Listener listener;
        const ChildResult result = inChild([&] { return connectToLoopback(listener.port(), Mode::Off); });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("inet"), "0");
        EXPECT_EQ(result.field("connect"), "0");
        EXPECT_EQ(result.field("netlink"), "0");
        EXPECT_TRUE(listener.connected());
    }

    // ---- Files --------------------------------------------------------------------------------

    struct Folders
    {
        TestArea area;
        fs::path input;  // folder A: the input of the worker
        fs::path other;  // folder B: somebody else's files
        fs::path output; // the output folder of the worker

        explicit Folders(const std::string &name)
            : area(name, true), input(area.getFolder("a")), other(area.getFolder("b")), output(area.getFolder("out"))
        {
            std::ofstream(input / "in.txt") << "input";
            std::ofstream(other / "secret.txt") << "secret";
        }
    };

    // The errno of the open of `path` for `flags`, 0 when it opened
    int openError(const fs::path &path, int flags)
    {
        const int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
        if (fd < 0)
            return errno;
        close(fd);
        return 0;
    }

    std::string touchFiles(const Folders &f, Mode mode)
    {
        sandbox::Policy policy;
        policy.readPaths = {io::toUtf8(f.input)};
        policy.writePaths = {io::toUtf8(f.output)};
        Fields out;
        const std::string refused = applyOrExplain(policy, mode);
        out.set("applied", refused.empty() ? "yes" : refused);

        char text[16] = {};
        const int input = open((f.input / "in.txt").c_str(), O_RDONLY);
        out.set("readInput", input < 0 ? std::string(std::strerror(errno)) : std::string(text, static_cast<size_t>(std::max<ssize_t>(0, read(input, text, sizeof(text) - 1)))));
        if (input >= 0)
            close(input);
        out.set("readOther", openError(f.other / "secret.txt", O_RDONLY));
        out.set("listOther", opendir(f.other.c_str()) == nullptr ? errno : 0);
        out.set("writeOutput", openError(f.output / "result.txt", O_WRONLY | O_CREAT | O_TRUNC));
        out.set("writeInput", openError(f.input / "new.txt", O_WRONLY | O_CREAT | O_TRUNC));
        out.set("rewriteInput", openError(f.input / "in.txt", O_WRONLY));
        out.set("unlinkInput", unlink((f.input / "in.txt").c_str()) == 0 ? 0 : errno);
        out.set("writeOther", openError(f.other / "new.txt", O_WRONLY | O_CREAT | O_TRUNC));
        return out.str();
    }

    TEST(Sandbox, aWorkerReadsItsInputFolderAndWritesItsOutputFolderOnly)
    {
        SKIP_WITHOUT_LAYER("landlock");
        const Folders f(TEST_NAME);
        const ChildResult result = inChild([&] { return touchFiles(f, Mode::BestEffort); });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("applied"), "yes");
        EXPECT_EQ(result.field("readInput"), "input");
        EXPECT_EQ(result.field("writeOutput"), "0");
        EXPECT_TRUE(fs::exists(f.output / "result.txt"));

        const std::string denied = std::to_string(EACCES);
        EXPECT_EQ(result.field("readOther"), denied);
        EXPECT_EQ(result.field("listOther"), denied);
        EXPECT_EQ(result.field("writeInput"), denied);
        EXPECT_EQ(result.field("rewriteInput"), denied);
        EXPECT_EQ(result.field("unlinkInput"), denied);
        EXPECT_EQ(result.field("writeOther"), denied);
        EXPECT_TRUE(fs::exists(f.input / "in.txt"));
        EXPECT_FALSE(fs::exists(f.input / "new.txt"));
        EXPECT_FALSE(fs::exists(f.other / "new.txt"));
    }

    TEST(Sandbox, withTheSandboxOffTheSameCodeTouchesEveryFolder)
    {
        const Folders f(TEST_NAME);
        const ChildResult result = inChild([&] { return touchFiles(f, Mode::Off); });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("readOther"), "0");
        EXPECT_EQ(result.field("listOther"), "0");
        EXPECT_EQ(result.field("writeInput"), "0");
        EXPECT_EQ(result.field("writeOther"), "0");
    }

    TEST(Sandbox, aReadPathThatDoesNotExistIsSkippedAndAWritePathThatDoesNotExistIsAnError)
    {
        SKIP_WITHOUT_LAYER("landlock");
        Folders f(TEST_NAME);
        const ChildResult result = inChild([&] {
            Fields out;
            sandbox::Policy skipped;
            skipped.readPaths = {io::toUtf8(f.area.getPath("not-there")), io::toUtf8(f.input)};
            const sandbox::Report ok = sandbox::apply(skipped, Mode::BestEffort);
            out.set("skippedOk", ok.ok ? 1 : 0);
            out.set("skippedReads", openError(f.input / "in.txt", O_RDONLY));
            return out.str();
        });
        EXPECT_EQ(result.field("skippedOk"), "1");
        EXPECT_EQ(result.field("skippedReads"), "0");

        const std::string missing = io::toUtf8(f.area.getPath("not-there"));
        const ChildResult failed = inChild([&] {
            sandbox::Policy bad;
            bad.writePaths = {missing};
            const sandbox::Report report = sandbox::apply(bad, Mode::BestEffort);
            return Fields().set("ok", report.ok ? 1 : 0).set("error", report.error).str();
        });
        EXPECT_EQ(failed.field("ok"), "0");
        EXPECT_NE(failed.field("error").find("not-there"), std::string::npos) << failed.field("error");
    }

    // ---- Resource limits ----------------------------------------------------------------------

    uint64_t addressSpaceNow()
    {
        std::ifstream statm("/proc/self/statm");
        uint64_t pages = 0;
        statm >> pages;
        return pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    }

    std::string allocateAGigabyte(Mode mode)
    {
        sandbox::Policy policy;
        policy.maxMemoryBytes = addressSpaceNow() + (256ULL << 20);
        const std::string refused = applyOrExplain(policy, mode);
        constexpr size_t Gigabyte = 1ULL << 30;
        void *block = mmap(nullptr, Gigabyte, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Fields out;
        out.set("applied", refused.empty() ? "yes" : refused);
        out.set("mmap", block == MAP_FAILED ? errno : 0);
        if (block != MAP_FAILED)
            munmap(block, Gigabyte);
        // Through a pointer the compiler cannot see through: it may drop a malloc whose result is
        // only compared and freed
        void *(*volatile allocate)(size_t) = std::malloc;
        void *heap = allocate(Gigabyte);
        out.set("malloc", heap == nullptr ? "null" : "block");
        std::free(heap);
        return out.str();
    }

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANDBOX_TEST_UNDER_SANITIZER 1
#endif

    TEST(Sandbox, anAllocationAboveTheAddressSpaceLimitFails)
    {
#ifdef SANDBOX_TEST_UNDER_SANITIZER
        GTEST_SKIP() << "A sanitizer maps address space of its own";
#endif
        const ChildResult limited = inChild([] { return allocateAGigabyte(Mode::BestEffort); });
        ASSERT_TRUE(limited.exited());
        EXPECT_EQ(limited.field("applied"), "yes");
        EXPECT_EQ(limited.field("mmap"), std::to_string(ENOMEM));
        EXPECT_EQ(limited.field("malloc"), "null");

        // Without the limit the same allocation succeeds
        const ChildResult free = inChild([] { return allocateAGigabyte(Mode::Off); });
        EXPECT_EQ(free.field("mmap"), "0");
        EXPECT_EQ(free.field("malloc"), "block");
    }

    double cpuSeconds()
    {
        timespec now{};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
        return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
    }

    // A loop that stops by itself after `giveUp` seconds of CPU time
    std::string spin(Mode mode, double giveUp)
    {
        sandbox::Policy policy;
        policy.maxCpuSeconds = 1;
        applyOrExplain(policy, mode);
        volatile uint64_t counter = 0;
        while (cpuSeconds() < giveUp)
            counter = counter + 1;
        return Fields().set("finished", "yes").str();
    }

    TEST(Sandbox, aLoopThatNeverEndsIsStoppedBySigxcpu)
    {
        const ChildResult limited = inChild([] { return spin(Mode::BestEffort, 30); });
        EXPECT_TRUE(limited.killedBy(SIGXCPU)) << "status " << limited.status;
        EXPECT_EQ(limited.field("finished"), "<missing>");

        // Without the limit the loop is still running when it gives up
        const ChildResult free = inChild([] { return spin(Mode::Off, 1.6); });
        ASSERT_TRUE(free.exited());
        EXPECT_EQ(free.field("finished"), "yes");
    }

    std::string limitsInForce(Mode mode)
    {
        sandbox::Policy policy;
        policy.maxMemoryBytes = addressSpaceNow() + (512ULL << 20);
        policy.maxCpuSeconds = 100;
        policy.maxFileBytes = 1 << 20;
        policy.maxOpenFiles = 64;
        applyOrExplain(policy, mode);
        Fields out;
        const std::pair<const char *, int> resources[] = {{"as", RLIMIT_AS},
                                                          {"cpu", RLIMIT_CPU},
                                                          {"fsize", RLIMIT_FSIZE},
                                                          {"nofile", RLIMIT_NOFILE},
                                                          {"core", RLIMIT_CORE}};
        for (const auto &[name, resource] : resources)
        {
            rlimit limit{};
            getrlimit(resource, &limit);
            out.set(std::string(name) + "Soft", limit.rlim_cur == RLIM_INFINITY ? -1 : static_cast<long long>(limit.rlim_cur));
            out.set(std::string(name) + "Hard", limit.rlim_max == RLIM_INFINITY ? -1 : static_cast<long long>(limit.rlim_max));
        }
        return out.str();
    }

    TEST(Sandbox, theFiveLimitsAreSetSoftAndHard)
    {
#ifdef SANDBOX_TEST_UNDER_SANITIZER
        GTEST_SKIP() << "A sanitizer maps address space of its own";
#endif
        const ChildResult limited = inChild([] { return limitsInForce(Mode::BestEffort); });
        EXPECT_NE(limited.field("asSoft"), "-1");
        EXPECT_EQ(limited.field("asSoft"), limited.field("asHard"));
        EXPECT_EQ(limited.field("cpuSoft"), "100");
        EXPECT_EQ(limited.field("cpuHard"), "101"); // SIGKILL a second after SIGXCPU
        EXPECT_EQ(limited.field("fsizeSoft"), std::to_string(1 << 20));
        EXPECT_EQ(limited.field("fsizeHard"), std::to_string(1 << 20));
        EXPECT_EQ(limited.field("nofileSoft"), "64");
        EXPECT_EQ(limited.field("nofileHard"), "64");
        EXPECT_EQ(limited.field("coreSoft"), "0");
        EXPECT_EQ(limited.field("coreHard"), "0");

        const ChildResult free = inChild([] { return limitsInForce(Mode::Off); });
        EXPECT_EQ(free.field("cpuSoft"), "-1");
        EXPECT_EQ(free.field("fsizeSoft"), "-1");
    }

    // ---- Privileges and the seccomp filter ----------------------------------------------------

    TEST(Sandbox, theProcessCannotGainPrivilegesAndIsTheFirstToBeKilledOnOutOfMemory)
    {
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0)
            GTEST_SKIP() << "The test process already runs with no_new_privs";
        const auto run = [](Mode mode) {
            return inChild([mode] {
                sandbox::Policy policy;
                policy.readPaths = {"/proc/self/oom_score_adj"};
                const sandbox::Report report = sandbox::apply(policy, mode);
                std::string score;
                std::ifstream("/proc/self/oom_score_adj") >> score;
                return Fields()
                    .set("noNewPrivs", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0))
                    .set("oomScore", score)
                    .set("applied", report.ok ? 1 : 0)
                    .str();
            });
        };
        const ChildResult sandboxed = run(Mode::BestEffort);
        EXPECT_EQ(sandboxed.field("noNewPrivs"), "1");
        EXPECT_EQ(sandboxed.field("oomScore"), "1000");

        const ChildResult free = run(Mode::Off);
        EXPECT_EQ(free.field("noNewPrivs"), "0");
        EXPECT_NE(free.field("oomScore"), "1000");
    }

    void *doNothing(void *)
    {
        return nullptr;
    }

    // What a worker may and may not do once the filter is in. Without the sandbox only the calls
    // that harm nothing are made: the others would start a program, trace the test process or
    // create a namespace.
    std::string callEverything(Mode mode)
    {
        const std::string refused = applyOrExplain(sandbox::Policy(), mode);
        const bool harmful = mode != Mode::Off;
        Fields out;
        out.set("applied", refused.empty() ? "yes" : refused);
        const auto errorOf = [](long result) { return result < 0 ? errno : 0; };

        // Other processes
        out.set("killParent", errorOf(kill(getppid(), 0)));
        out.set("tgkillParent", errorOf(syscall(SYS_tgkill, getppid(), getppid(), 0)));
        out.set("pidfdOpen", errorOf(syscall(SYS_pidfd_open, getppid(), 0)));
        if (!harmful)
            return out.str();
        out.set("killGroup", errorOf(kill(0, 0)));
        out.set("killEveryone", errorOf(kill(-1, 0)));
        out.set("killSelf", errorOf(kill(getpid(), 0)));
        out.set("tgkillSelf", errorOf(syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), 0)));
        out.set("ptrace", errorOf(ptrace(PTRACE_ATTACH, getppid(), nullptr, nullptr)));
        char byte = 0;
        iovec local{&byte, 1};
        iovec remote{&byte, 1};
        out.set("processVmReadv", errorOf(syscall(SYS_process_vm_readv, getppid(), &local, 1UL, &remote, 1UL, 0UL)));
        out.set("processVmWritev", errorOf(syscall(SYS_process_vm_writev, getppid(), &local, 1UL, &remote, 1UL, 0UL)));

        // Programs
        char *const noArguments[] = {nullptr};
        out.set("execve", errorOf(execve("/bin/true", noArguments, noArguments)));
        out.set("execveat", errorOf(syscall(SYS_execveat, AT_FDCWD, "/bin/true", noArguments, noArguments, 0)));

        // Namespaces and mounts
        out.set("cloneNewUser", errorOf(syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, nullptr, nullptr, nullptr, nullptr)));
        out.set("cloneNewNet", errorOf(syscall(SYS_clone, CLONE_NEWNET | SIGCHLD, nullptr, nullptr, nullptr, nullptr)));
        out.set("clone3", errorOf(syscall(SYS_clone3, nullptr, 0)));
        out.set("unshare", errorOf(unshare(CLONE_NEWNS)));
        out.set("setns", errorOf(setns(-1, 0)));
        out.set("mount", errorOf(mount("none", "/", nullptr, MS_REMOUNT, nullptr)));

        // Kernel interfaces
        out.set("ioUringSetup", errorOf(syscall(SYS_io_uring_setup, 1, nullptr)));
        out.set("bpf", errorOf(syscall(SYS_bpf, 0, nullptr, 0)));
        out.set("perfEventOpen", errorOf(syscall(SYS_perf_event_open, nullptr, 0, -1, -1, 0)));
        out.set("keyctl", errorOf(syscall(SYS_keyctl, 0, 0, 0, 0, 0)));
        out.set("addKey", errorOf(syscall(SYS_add_key, nullptr, nullptr, nullptr, 0, 0)));
        out.set("requestKey", errorOf(syscall(SYS_request_key, nullptr, nullptr, nullptr, 0)));
        out.set("userfaultfd", errorOf(syscall(SYS_userfaultfd, 0)));
        return out.str();
    }

    TEST(Sandbox, theFilterRefusesWhatAnExploitNeedsWithEperm)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        const ChildResult result = inChild([] { return callEverything(Mode::BestEffort); });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("applied"), "yes");
        const std::string denied = std::to_string(EPERM);
        for (const char *call : {"execve", "execveat", "killParent", "killGroup", "killEveryone", "tgkillParent",
                                 "ptrace", "processVmReadv", "processVmWritev", "pidfdOpen", "cloneNewUser",
                                 "cloneNewNet", "unshare", "setns", "mount", "ioUringSetup", "bpf", "perfEventOpen",
                                 "keyctl", "addKey", "requestKey", "userfaultfd"})
            EXPECT_EQ(result.field(call), denied) << call;
        // The one thing a filter cannot see into: the C library falls back to clone
        EXPECT_EQ(result.field("clone3"), std::to_string(ENOSYS));
        // A signal to the process itself is its own business
        EXPECT_EQ(result.field("killSelf"), "0");
        EXPECT_EQ(result.field("tgkillSelf"), "0");
    }

    // The calls that reach another process work without the filter
    TEST(Sandbox, withTheSandboxOffTheSameCallsReachOtherProcesses)
    {
        const ChildResult result = inChild([] { return callEverything(Mode::Off); });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("killParent"), "0");
        EXPECT_EQ(result.field("tgkillParent"), "0");
        EXPECT_EQ(result.field("pidfdOpen"), "0");
    }

    TEST(Sandbox, forkAndThreadsStillWork)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        const ChildResult result = inChild([] {
            Fields out;
            out.set("applied", applyOrExplain(sandbox::Policy(), Mode::BestEffort).empty() ? "yes" : "no");

            const pid_t grandchild = fork();
            if (grandchild == 0)
                _exit(7);
            int status = 0;
            waitpid(grandchild, &status, 0);
            out.set("fork", WIFEXITED(status) ? WEXITSTATUS(status) : -1);

            pthread_t thread;
            out.set("thread", pthread_create(&thread, nullptr, doNothing, nullptr) == 0 && pthread_join(thread, nullptr) == 0
                                  ? "ran"
                                  : "failed");
            // The child of a fork is another process: a signal to it is refused, as to any
            // process but the worker itself
            int channel[2];
            if (pipe(channel) != 0)
                return out.str();
            const pid_t sibling = fork();
            if (sibling == 0)
            {
                // Waits until the parent closes its end of the pipe
                close(channel[1]);
                char go = 0;
                _exit(read(channel[0], &go, 1) == 0 ? 0 : 1);
            }
            out.set("killChild", kill(sibling, 0) == 0 ? 0 : errno);
            close(channel[1]);
            waitpid(sibling, &status, 0);
            return out.str();
        });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("applied"), "yes");
        EXPECT_EQ(result.field("fork"), "7");
        EXPECT_EQ(result.field("thread"), "ran");
        EXPECT_EQ(result.field("killChild"), std::to_string(EPERM));
    }

#if defined(__x86_64__)
    // A syscall of the x32 ABI or of the 32-bit ABI kills the process: the numbers the filter knows
    // are the native ones
    TEST(Sandbox, aSyscallOfAnotherAbiKillsTheProcess)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        const ChildResult result = inChild([] {
            applyOrExplain(sandbox::Policy(), Mode::BestEffort);
            Fields out;
            int status = 0;
            const pid_t x32 = fork();
            if (x32 == 0)
            {
                syscall(SYS_getpid | 0x40000000);
                _exit(0);
            }
            waitpid(x32, &status, 0);
            out.set("x32", WIFSIGNALED(status) ? WTERMSIG(status) : 0);

            const pid_t native = fork();
            if (native == 0)
            {
                syscall(SYS_getpid);
                _exit(0);
            }
            waitpid(native, &status, 0);
            out.set("native", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return out.str();
        });
        EXPECT_EQ(result.field("x32"), std::to_string(SIGSYS));
        EXPECT_EQ(result.field("native"), "0");
    }
#endif

    // ---- Modes ---------------------------------------------------------------------------------

    TEST(Sandbox, strictRefusesWhenALayerIsMissingAndNamesItAndAppliesNothing)
    {
        const ChildResult result = inChild([] {
            sandbox::Policy policy;
            policy.unavailableLayers = {"landlock"};
            const sandbox::Report report = sandbox::apply(policy, Mode::Strict);
            return Fields()
                .set("ok", report.ok ? 1 : 0)
                .set("error", report.error)
                .set("missing", report.missingLayers().empty() ? "" : report.missingLayers().front())
                .set("noNewPrivs", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0))
                .str();
        });
        ASSERT_TRUE(result.exited());
        EXPECT_EQ(result.field("ok"), "0");
        EXPECT_NE(result.field("error").find("landlock"), std::string::npos) << result.field("error");
        EXPECT_EQ(result.field("missing"), "landlock");
        // Strict checked before it applied: the process is not restricted
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 0)
        {
            EXPECT_EQ(result.field("noNewPrivs"), "0");
        }
    }

    TEST(Sandbox, strictOnAFullyCapableHostApplies)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        SKIP_WITHOUT_LAYER("landlock");
        SKIP_WITHOUT_LAYER("oom-score");
        const ChildResult result = inChild([] {
            const sandbox::Report report = sandbox::apply(sandbox::Policy(), Mode::Strict);
            return Fields().set("ok", report.ok ? 1 : 0).set("error", report.error).set("missing", report.missingLayers().size()).str();
        });
        EXPECT_EQ(result.field("ok"), "1") << result.field("error");
        EXPECT_EQ(result.field("missing"), "0");
    }

    TEST(Sandbox, bestEffortAppliesTheOtherLayersAndNamesTheMissingOneOnce)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        const ChildResult result = inChild([] {
            sandbox::Policy policy;
            policy.unavailableLayers = {"landlock"};
            const sandbox::Report report = sandbox::apply(policy, Mode::BestEffort);
            const std::string warning = report.warning();
            size_t mentions = 0;
            for (size_t at = warning.find("landlock"); at != std::string::npos; at = warning.find("landlock", at + 1))
                mentions++;
            // The seccomp layer is on: a network socket is refused
            const int inet = socket(AF_INET, SOCK_STREAM, 0);
            return Fields()
                .set("ok", report.ok ? 1 : 0)
                .set("missing", report.missingLayers().size())
                .set("first", report.missingLayers().empty() ? "" : report.missingLayers().front())
                .set("mentions", mentions)
                .set("socket", inet < 0 ? errno : 0)
                .str();
        });
        EXPECT_EQ(result.field("ok"), "1");
        EXPECT_EQ(result.field("missing"), "1");
        EXPECT_EQ(result.field("first"), "landlock");
        EXPECT_EQ(result.field("mentions"), "1");
        EXPECT_EQ(result.field("socket"), std::to_string(EPERM));
    }

    // ---- GDAL under the sandbox ---------------------------------------------------------------
    //
    // GDAL opens more than the file it is given: the datasets the metadata of a raster names, the
    // sources of a VRT, the schemas a GML includes. The open policy of libddb refuses those
    // (gdal_open_policy.cpp), but a plain GDALOpenEx does not, and the sandbox has to contain it
    // on its own: the worker runs GDAL on a file of a user.

    constexpr double Secret = 4242.0;

    fs::path writeRaster(const fs::path &path, const std::function<double(int x, int y)> &value)
    {
        testutils::GeoRasterSpec spec;
        spec.width = spec.height = 32;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.epsg = 32615;
        spec.originX = 576648.71;
        spec.originY = 5188209.785;
        spec.pixelSize = 0.05;
        spec.value = [value](int, int x, int y) { return value(x, y); };
        return testutils::createGeoRaster(path, spec);
    }

    fs::path writeRaster(const fs::path &path, double value)
    {
        return writeRaster(path, [value](int, int) { return value; });
    }

    void writeText(const fs::path &path, const std::string &text)
    {
        std::ofstream(path, std::ios::binary) << text;
    }

    // The folders GDAL needs to read to work at all
    std::vector<std::string> gdalDataFolders()
    {
        std::vector<std::string> folders;
        for (const char *option : {"GDAL_DATA", "PROJ_DATA", "PROJ_LIB"})
            if (const char *value = CPLGetConfigOption(option, nullptr))
                folders.push_back(value);
        const PJ_INFO info = proj_info();
        if (info.searchpath != nullptr)
        {
            std::istringstream paths(info.searchpath);
            std::string path;
            while (std::getline(paths, path, ':'))
                if (!path.empty())
                    folders.push_back(path);
        }
        for (const char *system : {"/dev/null", "/dev/urandom", "/proc/self", "/proc/meminfo", "/etc/localtime"})
            folders.push_back(system);
        return folders;
    }

    struct BypassFiles
    {
        TestArea area;
        fs::path input;
        fs::path other;
        fs::path overviewTif; // names an overview file in the other folder
        fs::path sourceVrt;   // reads a source in the other folder
        fs::path warpedVrt;   // a geolocation array in the other folder
        fs::path gml;         // an .xsd that includes a /vsicurl/ schema
        fs::path output;

        explicit BypassFiles(const std::string &name)
            : area(name, true), input(area.getFolder("a")), other(area.getFolder("b")), output(area.getFolder("out"))
        {
            const fs::path outsideRaster = writeRaster(other / "secret.tif", Secret);
            const std::string outside = io::toGenericUtf8(outsideRaster);

            overviewTif = writeRaster(input / "overviews.tif", 1.0);
            {
                const auto dataset = adoptDataset(GDALOpen(io::toUtf8(overviewTif).c_str(), GA_Update));
                if (dataset == nullptr || dataset->SetMetadataItem("OVERVIEW_FILE", outside.c_str(), "OVERVIEWS") != CE_None)
                    throw std::runtime_error("cannot write the metadata of the GeoTIFF");
            }

            sourceVrt = input / "source.vrt";
            writeText(sourceVrt, vrtReading(outside));

            // Geolocation arrays (longitude and latitude of every pixel) in the other folder
            writeRaster(input / "a.tif", 1.0);
            const fs::path longitudes = writeRaster(other / "lon.tif", [](int x, int) { return 10.0 + x * 0.001; });
            const fs::path latitudes = writeRaster(other / "lat.tif", [](int, int y) { return 45.0 - y * 0.001; });
            warpedVrt = input / "warped.vrt";
            writeText(warpedVrt, warpedVrtWithArrays(io::toGenericUtf8(longitudes), io::toGenericUtf8(latitudes)));
        }

        void writeGml(int port)
        {
            gml = input / "schema.gml";
            writeText(gml, "<?xml version=\"1.0\"?>\n<ns:FeatureCollection xmlns:ns=\"http://example.com/ns\" "
                           "xmlns:gml=\"http://www.opengis.net/gml\">\n  <gml:featureMember><ns:t><ns:name>a</ns:name>"
                           "<ns:geom><gml:Point srsName=\"EPSG:4326\"><gml:coordinates>10,45</gml:coordinates></gml:Point>"
                           "</ns:geom></ns:t></gml:featureMember>\n</ns:FeatureCollection>\n");
            writeText(input / "schema.xsd",
                      "<?xml version=\"1.0\"?>\n<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
                      "targetNamespace=\"http://example.com/ns\">\n  <xs:include schemaLocation=\"/vsicurl/http://127.0.0.1:" +
                          std::to_string(port) + "/schema.tif\"/>\n</xs:schema>\n");
        }

        static std::string vrtReading(const std::string &source)
        {
            return "<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\">\n  <SRS>EPSG:32615</SRS>\n"
                   "  <GeoTransform>576648.71, 0.05, 0, 5188209.785, 0, -0.05</GeoTransform>\n"
                   "  <VRTRasterBand dataType=\"Float32\" band=\"1\">\n    <ComplexSource>\n"
                   "      <SourceFilename relativeToVRT=\"0\">" +
                   source +
                   "</SourceFilename>\n      <SourceBand>1</SourceBand>\n"
                   "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"32\" ySize=\"32\" />\n"
                   "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"32\" ySize=\"32\" />\n"
                   "    </ComplexSource>\n  </VRTRasterBand>\n</VRTDataset>\n";
        }

        // A warped VRT over a.tif whose geolocation transformer reads its arrays from two rasters
        static std::string warpedVrtWithArrays(const std::string &longitudes, const std::string &latitudes)
        {
            return "<VRTDataset rasterXSize=\"32\" rasterYSize=\"32\" subClass=\"VRTWarpedDataset\">\n"
                   "  <SRS>EPSG:32615</SRS>\n"
                   "  <GeoTransform>576648.71, 0.05, 0, 5188209.785, 0, -0.05</GeoTransform>\n"
                   "  <BlockXSize>32</BlockXSize>\n  <BlockYSize>32</BlockYSize>\n"
                   "  <VRTRasterBand dataType=\"Float32\" band=\"1\" subClass=\"VRTWarpedRasterBand\" />\n"
                   "  <GDALWarpOptions>\n    <WarpMemoryLimit>67108864</WarpMemoryLimit>\n"
                   "    <ResampleAlg>NearestNeighbour</ResampleAlg>\n    <WorkingDataType>Float32</WorkingDataType>\n"
                   "    <SourceDataset relativeToVRT=\"1\">a.tif</SourceDataset>\n"
                   "    <Transformer><GeoLocTransformer><Reversed>0</Reversed><Metadata>"
                   "<MDI key=\"X_DATASET\">" +
                   longitudes + "</MDI><MDI key=\"X_BAND\">1</MDI><MDI key=\"Y_DATASET\">" + latitudes +
                   "</MDI><MDI key=\"Y_BAND\">1</MDI><MDI key=\"PIXEL_OFFSET\">0</MDI><MDI key=\"LINE_OFFSET\">0</MDI>"
                   "<MDI key=\"PIXEL_STEP\">1</MDI><MDI key=\"LINE_STEP\">1</MDI></Metadata></GeoLocTransformer>"
                   "</Transformer>\n    <BandList><BandMapping src=\"1\" dst=\"1\" /></BandList>\n"
                   "  </GDALWarpOptions>\n</VRTDataset>\n";
        }
    };

    // The value at the first pixel of `band`, or "unreadable"
    std::string firstPixel(GDALRasterBand *band)
    {
        double value = 0;
        if (band == nullptr || band->RasterIO(GF_Read, 0, 0, 1, 1, &value, 1, 1, GDT_Float64, 0, 0) != CE_None)
            return "unreadable";
        return std::to_string(static_cast<long long>(value));
    }

    // Opens the three files of the scene with a plain GDALOpenEx, the way a worker that did not
    // use the open policy would, under the sandbox (or not)
    std::string openWithPlainGdal(const BypassFiles &f, Mode mode)
    {
        // Everything GDAL loads lazily is loaded before the file system is closed, but the
        // folders it reads from stay readable
        CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "2");
        CPLSetConfigOption("GDAL_HTTP_MAX_RETRY", "0");
        CPLSetConfigOption("CPL_CURL_VERBOSE", "NO");

        sandbox::Policy policy;
        policy.readPaths = gdalDataFolders();
        policy.readPaths.push_back(io::toUtf8(f.input));
        policy.writePaths = {io::toUtf8(f.output)};
        Fields out;
        const std::string refused = applyOrExplain(policy, mode);
        out.set("applied", refused.empty() ? "yes" : refused);

        // The overview a raster names in its metadata
        {
            const auto dataset = adoptDataset(
                GDALOpenEx(io::toUtf8(f.overviewTif).c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
            out.set("overviewRaster", dataset == nullptr ? "no" : "yes");
            std::string value = "none";
            if (dataset != nullptr)
            {
                GDALRasterBand *band = dataset->GetRasterBand(1);
                if (band->GetOverviewCount() > 0)
                    value = firstPixel(band->GetOverview(0));
            }
            out.set("overview", value);
        }
        // A VRT that reads a source outside its folder
        {
            const auto dataset = adoptDataset(
                GDALOpenEx(io::toUtf8(f.sourceVrt).c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
            out.set("vrt", dataset == nullptr ? "no dataset" : firstPixel(dataset->GetRasterBand(1)));
        }
        // A warped VRT whose geolocation arrays are outside its folder
        {
            CPLPushErrorHandler(CPLQuietErrorHandler);
            const auto dataset = adoptDataset(
                GDALOpenEx(io::toUtf8(f.warpedVrt).c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
            CPLPopErrorHandler();
            out.set("warped", dataset == nullptr ? "no" : "yes");
            out.set("warpedError", dataset == nullptr ? CPLGetLastErrorMsg() : "");
        }
        // A GML whose schema includes a URL
        {
            CPLPushErrorHandler(CPLQuietErrorHandler);
            const auto dataset = adoptDataset(
                GDALOpenEx(io::toUtf8(f.gml).c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
            CPLPopErrorHandler();
            out.set("gml", dataset == nullptr ? "no" : "yes");
        }
        return out.str();
    }

    TEST(Sandbox, plainGdalCannotLeaveTheInputFolderOrReachTheNetwork)
    {
        SKIP_WITHOUT_LAYER("seccomp");
        SKIP_WITHOUT_LAYER("landlock");
        BypassFiles f(TEST_NAME);
        Listener listener;
        f.writeGml(listener.port());

        const ChildResult result = inChild([&] { return openWithPlainGdal(f, Mode::BestEffort); });
        ASSERT_TRUE(result.exited()) << "status " << result.status;
        ASSERT_EQ(result.field("applied"), "yes");
        // The files in the folder open, since the worker was given that folder
        EXPECT_EQ(result.field("overviewRaster"), "yes");
        // ... but nothing they name outside it is read
        EXPECT_EQ(result.field("overview"), "none");
        EXPECT_NE(result.field("vrt"), std::to_string(static_cast<long long>(Secret)));
        EXPECT_EQ(result.field("warped"), "no");
        EXPECT_FALSE(listener.connected());
    }

    // The same files without the sandbox: GDAL reads the outside raster and asks the network
    TEST(Sandbox, withTheSandboxOffPlainGdalFollowsEveryReference)
    {
        BypassFiles f(TEST_NAME);
        Listener listener;
        f.writeGml(listener.port());

        const ChildResult result = inChild([&] { return openWithPlainGdal(f, Mode::Off); });
        ASSERT_TRUE(result.exited()) << "status " << result.status;
        const std::string secret = std::to_string(static_cast<long long>(Secret));
        EXPECT_EQ(result.field("overview"), secret);
        EXPECT_EQ(result.field("vrt"), secret);
        EXPECT_EQ(result.field("warped"), "yes") << result.field("warpedError");
        EXPECT_TRUE(listener.connected());
    }
} // namespace

#endif // __linux__
