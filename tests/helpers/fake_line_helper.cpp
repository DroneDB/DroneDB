/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A stand-in for any program that speaks in lines, for the tests of the generic helper pool
// (helper_pool.h). `--version` prints one line, `--serve` answers each request line on stdin with
// one line on stdout until stdin ends. The request says what the answer is:
//
//   echo <text>        answers <text>
//   pid                answers the process id
//   env <NAME>         answers the value of the environment variable, "unset" when it has none
//   rlimit             answers the soft limit on the address space ("unlimited", or bytes;
//                      "unsupported" where the system has no such limit)
//   huge <bytes>       answers a line of that many bytes
//   crash              exits in the middle of the request, without answering
//   hang               never answers
//   hold <file>        waits until <file> exists, then answers "released"
//   anything else      answers the request as it came
//
// Environment, read when the program starts:
//   DDB_FAKE_LINE_VERSION          what `--version` prints (default "fake-line 1.0")
//   DDB_FAKE_LINE_VERSION_LOG      a file that every run of `--version` appends "<pid>" to
//   DDB_FAKE_LINE_STAY_ON_EOF=1    the helper does not end when its stdin does (a pool kills it)
//   DDB_FAKE_LINE_EXIT_STATUS      the exit code once stdin ended (default 0)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#define FAKE_GETPID _getpid
#else
#include <sys/resource.h>
#include <unistd.h>
#define FAKE_GETPID getpid
#endif

namespace {

std::string envOr(const char *name, const std::string &fallback) {
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

bool startsWith(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}

void answer(const std::string &text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string addressSpaceLimit() {
#ifdef _WIN32
    return "unsupported";
#else
    rlimit limit{};
    if (getrlimit(RLIMIT_AS, &limit) != 0) return "unsupported";
    return limit.rlim_cur == RLIM_INFINITY ? "unlimited" : std::to_string(static_cast<unsigned long long>(limit.rlim_cur));
#endif
}

int serve() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (startsWith(line, "echo ")) {
            answer(line.substr(5));
        } else if (line == "pid") {
            answer(std::to_string(FAKE_GETPID()));
        } else if (startsWith(line, "env ")) {
            answer(envOr(line.substr(4).c_str(), "unset"));
        } else if (line == "rlimit") {
            answer(addressSpaceLimit());
        } else if (startsWith(line, "huge ")) {
            const size_t bytes = static_cast<size_t>(std::stoull(line.substr(5)));
            const std::string chunk(64 * 1024, 'x');
            for (size_t written = 0; written < bytes; written += chunk.size())
                std::fwrite(chunk.data(), 1, std::min(chunk.size(), bytes - written), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else if (line == "crash") {
            std::_Exit(7);
        } else if (line == "hang") {
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        } else if (startsWith(line, "hold ")) {
            const std::string release = line.substr(5);
            while (!std::ifstream(release).good()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            answer("released");
        } else {
            answer(line);
        }
    }
    if (envOr("DDB_FAKE_LINE_STAY_ON_EOF", "") == "1")
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    return std::atoi(envOr("DDB_FAKE_LINE_EXIT_STATUS", "0").c_str());
}

}  // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--version") {
        const std::string log = envOr("DDB_FAKE_LINE_VERSION_LOG", "");
        if (!log.empty()) std::ofstream(log, std::ios::app) << FAKE_GETPID() << "\n";
        std::printf("%s\n", envOr("DDB_FAKE_LINE_VERSION", "fake-line 1.0").c_str());
        return 0;
    }
    if (mode == "--serve") return serve();
    std::fprintf(stderr, "usage: ddbtest_fake_line --version | --serve\n");
    return 64;
}
