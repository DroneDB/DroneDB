/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Deterministic child modes for process supervision and lock crash tests.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "buildlock.h"
#include "fs.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using std::chrono::milliseconds;

void delay(int ms) { std::this_thread::sleep_for(milliseconds(ms)); }

void closeOutput() {
#ifdef _WIN32
    // stdout/stderr use the same inherited pipe HANDLE. Close the CRT handles
    // and the OS handles (a CRT implementation may have duplicated them).
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    _close(1);
    _close(2);
    CloseHandle(out);
    if (err != out) CloseHandle(err);
    SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
    SetStdHandle(STD_ERROR_HANDLE, nullptr);
#else
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
#endif
}

void writeExtra(const std::string& id, const char* data, size_t length) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(id)));
    DWORD written = 0;
    WriteFile(h, data, static_cast<DWORD>(length), &written, nullptr);
#else
    const ssize_t written = ::write(std::stoi(id), data, length);
    (void)written;
#endif
}

int descendantWorker(const std::string& activity, const std::string& extraId) {
    const std::string chunk(64 * 1024, 'x');
    for (;;) {
        if (activity == "streaming") {
            std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fflush(stdout);
        } else if (activity == "extra") {
            writeExtra(extraId, chunk.data(), chunk.size());
        }
        delay(1);
    }
}

unsigned long spawnDescendant(const fs::path& self, const std::string& activity,
                              const std::string& extraId) {
#ifdef _WIN32
    const std::wstring executable = fs::absolute(self).wstring();
    const std::string mode = " descendant-worker " + activity + " " + extraId;
    std::wstring command = L"\"" + executable + L"\"" + std::wstring(mode.begin(), mode.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 0;
    const DWORD pid = child.dwProcessId;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return pid;
#else
    (void)self;
    const pid_t child = ::fork();
    if (child == 0) ::_exit(descendantWorker(activity, extraId));
    return child < 0 ? 0 : static_cast<unsigned long>(child);
#endif
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    // External tool runners control their arguments. An environment override
    // lets their tests use this real executable without needing shell scripts.
    if (const char* overrideMode = std::getenv("DDB_TEST_CHILD_MODE")) {
        if (std::string(overrideMode) == "no-output") return 0;
        if (std::string(overrideMode) == "idle") for (;;) delay(1000);
    }
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "exit") {
        if (argc < 3) return 2;
        std::fputs("hello\n", stdout);
        return std::stoi(argv[2]);
    }
    if (mode == "close-output") {
        if (argc < 4) return 2;
        std::fputs("before-close\n", stdout);
        std::fflush(stdout);
        closeOutput();
        if (argc > 4) {
#ifdef _WIN32
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[4]))));
#else
            ::close(std::stoi(argv[4]));
#endif
        }
        delay(std::stoi(argv[2]));
        return std::stoi(argv[3]);
    }
    if (mode == "extra-ticks") {
        if (argc < 3) return 2;
        for (int i = 0; i < 8; ++i) {
            writeExtra(argv[2], "tick\n", 5);
            delay(200);
        }
        return 0;
    }
    if (mode == "descendant-worker")
        return argc < 3 ? 2 : descendantWorker(argv[2], argc > 3 ? argv[3] : "");
    if (mode == "descendant") {
        // descendant PID file, output mode, direct-child lifetime, direct exit code,
        // optional inherited extra-channel handle/fd.
        if (argc < 6) return 2;
        const auto pid = spawnDescendant(argv[0], argv[3], argc > 6 ? argv[6] : "");
        if (!pid) return 3;
        {
            std::ofstream ready(argv[2]);
            ready << pid;
        }
        delay(std::stoi(argv[4]));
        return std::stoi(argv[5]);
    }
    if (mode == "hold-lock") {
        if (argc < 4) return 2;
        try {
            ddb::BuildLock lock(argv[2], false);
            {
                std::ofstream ready(argv[3]);
                ready << "ready";
            }
            for (;;) delay(1000);
        } catch (...) {
            return 1;
        }
    }
    return 2;
}
