/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "buildlod_runner.h"

#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "logger.h"
#include "mio.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
extern char **environ;
#endif

namespace ddb
{
    namespace buildlod
    {

        namespace
        {

#ifdef _WIN32
            constexpr const char *kBinaryName = "build-lod.exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kBinaryName = "build-lod";
            constexpr char kPathSep = ':';
#endif

            std::mutex g_cacheMutex;
            bool g_cacheValid = false;
            fs::path g_cachedBinary;

            bool isExecutableFile(const fs::path &p)
            {
                std::error_code ec;
                if (!fs::exists(p, ec) || ec) return false;
                if (!fs::is_regular_file(p, ec) || ec) return false;
#ifdef _WIN32
                return true;
#else
                auto perms = fs::status(p, ec).permissions();
                if (ec) return false;
                using fs::perms;
                return (perms & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
            }

            fs::path searchOnPath()
            {
                const char *path = std::getenv("PATH");
                if (!path || !*path) return {};

                std::string p(path);
                size_t start = 0;
                while (start <= p.size())
                {
                    size_t end = p.find(kPathSep, start);
                    if (end == std::string::npos) end = p.size();
                    std::string dir = p.substr(start, end - start);
                    if (!dir.empty())
                    {
                        fs::path candidate = fs::path(dir) / kBinaryName;
                        if (isExecutableFile(candidate)) return candidate;
                    }
                    if (end == p.size()) break;
                    start = end + 1;
                }
                return {};
            }

            std::string quoteArg(const std::string &arg)
            {
#ifdef _WIN32
                if (arg.find_first_of(" \t\"") == std::string::npos && !arg.empty())
                    return arg;

                std::string out;
                out.reserve(arg.size() + 2);
                out.push_back('"');
                size_t backslashes = 0;
                for (char c : arg)
                {
                    if (c == '\\')
                    {
                        ++backslashes;
                        out.push_back(c);
                    }
                    else if (c == '"')
                    {
                        out.append(backslashes + 1, '\\');
                        out.push_back('"');
                        backslashes = 0;
                    }
                    else
                    {
                        backslashes = 0;
                        out.push_back(c);
                    }
                }
                out.append(backslashes, '\\');
                out.push_back('"');
                return out;
#else
                return arg;
#endif
            }

            // build-lod derives its output name from the input: it replaces the input's
            // extension with "-lod" and appends ".rad" (see rust/build-lod/src/main.rs).
            // For "<dir>/model.spz" the produced file is "<dir>/model-lod.rad".
            fs::path expectedLodOutput(const fs::path &input)
            {
                fs::path stem = input.stem(); // "model.spz" -> "model"
                return input.parent_path() / (stem.string() + "-lod.rad");
            }

        } // namespace

        fs::path findBuildLodBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var (authoritative when non-empty).
            if (const char *override = std::getenv("DDB_BUILDLOD_PATH"))
            {
                if (override[0] != '\0')
                {
                    fs::path p(override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "build-lod binary discovered via DDB_BUILDLOD_PATH at " << found.string();
                    else
                        LOGD << "DDB_BUILDLOD_PATH=" << override << " is not an executable file; treating build-lod as unavailable";
                    return found;
                }
            }

            // 2. Folder of the current executable (where packaging bundles build-lod).
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty())
                {
                    fs::path candidate = exeDir / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 3. <install_prefix>/bin (parent of exe folder + /bin).
            if (found.empty())
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty() && exeDir.has_parent_path())
                {
                    fs::path candidate = exeDir.parent_path() / "bin" / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 4. System PATH.
            if (found.empty()) found = searchOnPath();

            g_cachedBinary = found;
            g_cacheValid = true;

            if (!found.empty())
                LOGD << "build-lod binary discovered at " << found.string();
            else
                LOGD << "build-lod binary not found in DDB_BUILDLOD_PATH, exe folder, install prefix or PATH";

            return found;
        }

        bool isBuildLodAvailable()
        {
            return !findBuildLodBinary().empty();
        }

        bool runBuildLod(const fs::path &input,
                         const fs::path &outputRad,
                         std::string &errorOut,
                         bool quality,
                         int maxSh,
                         const fs::path &binary)
        {
            errorOut.clear();

            fs::path bin = binary.empty() ? findBuildLodBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                errorOut = "build-lod binary not found";
                return false;
            }

            std::error_code ec;
            if (!fs::exists(input, ec) || ec)
            {
                errorOut = "build-lod input does not exist: " + input.string();
                return false;
            }

            const fs::path producedRad = expectedLodOutput(input);

            // Clean any stale outputs so we never mistake a leftover for a fresh build.
            fs::remove(producedRad, ec);
            fs::remove(outputRad, ec);
            fs::create_directories(outputRad.parent_path(), ec);

            // Clamp SH degree to the range build-lod accepts (0..3).
            int clampedSh = maxSh < 0 ? 0 : (maxSh > 3 ? 3 : maxSh);

            std::vector<std::string> args;
            args.push_back(bin.string());
            args.push_back(input.string());
            args.push_back(quality ? "--quality" : "--quick");
            args.push_back("--max-sh=" + std::to_string(clampedSh));
            args.push_back("--rad"); // single-file RAD (HTTP range friendly)

            {
                std::ostringstream cmdDbg;
                for (size_t i = 0; i < args.size(); ++i)
                {
                    if (i > 0) cmdDbg << ' ';
                    cmdDbg << quoteArg(args[i]);
                }
                LOGD << "Spawning build-lod: " << cmdDbg.str();
            }

#ifdef _WIN32
            // Capture the child's stdout+stderr through a single pipe so build-lod's
            // (verbose) progress output is forwarded to the DDB log instead of a console.
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;

            HANDLE hOutRead = nullptr, hOutWrite = nullptr;
            if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0))
            {
                errorOut = "CreatePipe failed for build-lod output";
                return false;
            }
            SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

            std::string cmdLine;
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (i > 0) cmdLine.push_back(' ');
                cmdLine.append(quoteArg(args[i]));
            }

            int wideLen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, nullptr, 0);
            std::wstring wideCmd(static_cast<size_t>(wideLen > 0 ? wideLen : 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, wideCmd.data(), wideLen);

            std::string binStr = bin.string();
            int wideBinLen = MultiByteToWideChar(CP_UTF8, 0, binStr.c_str(), -1, nullptr, 0);
            std::wstring wideBin(static_cast<size_t>(wideBinLen > 0 ? wideBinLen : 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, binStr.c_str(), -1, wideBin.data(), wideBinLen);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hOutWrite;
            si.hStdError = hOutWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            PROCESS_INFORMATION pi{};

            BOOL ok = CreateProcessW(
                wideBin.c_str(),
                wideCmd.data(),
                nullptr, nullptr,
                TRUE, // inherit hStdOutput/hStdError
                CREATE_NO_WINDOW,
                nullptr, nullptr,
                &si, &pi);

            CloseHandle(hOutWrite);

            if (!ok)
            {
                CloseHandle(hOutRead);
                DWORD err = GetLastError();
                std::ostringstream oss;
                oss << "CreateProcess failed for build-lod (error " << err << ")";
                errorOut = oss.str();
                return false;
            }

            std::string captured;
            {
                char buf[4096];
                DWORD nr = 0;
                while (ReadFile(hOutRead, buf, sizeof(buf), &nr, nullptr) && nr > 0)
                    captured.append(buf, nr);
            }
            CloseHandle(hOutRead);

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
#else
            int outPipe[2];
            if (pipe(outPipe) != 0)
            {
                errorOut = "pipe() failed for build-lod output";
                return false;
            }

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addclose(&actions, outPipe[0]);
            posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, outPipe[1]);

            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);

            pid_t pid = 0;
            int spawnRet = posix_spawn(&pid, bin.c_str(), &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);

            close(outPipe[1]);

            if (spawnRet != 0)
            {
                close(outPipe[0]);
                std::ostringstream oss;
                oss << "posix_spawn failed for build-lod (errno " << spawnRet << ")";
                errorOut = oss.str();
                return false;
            }

            std::string captured;
            {
                char buf[4096];
                ssize_t nr = 0;
                while ((nr = read(outPipe[0], buf, sizeof(buf))) > 0)
                    captured.append(buf, static_cast<size_t>(nr));
            }
            close(outPipe[0]);

            int status = 0;
            if (waitpid(pid, &status, 0) < 0)
            {
                errorOut = "waitpid failed for build-lod";
                return false;
            }
            if (!WIFEXITED(status))
            {
                errorOut = "build-lod terminated abnormally";
                return false;
            }
            int exitCode = WEXITSTATUS(status);
#endif

            if (!captured.empty())
                LOGD << "[build-lod] " << captured;

            if (exitCode != 0)
            {
                std::ostringstream oss;
                oss << "build-lod exited with code " << exitCode;
                // Surface the tail of the captured output to aid diagnostics.
                if (!captured.empty())
                {
                    constexpr size_t kTail = 512;
                    const std::string tail = captured.size() > kTail
                                                 ? captured.substr(captured.size() - kTail)
                                                 : captured;
                    oss << ": " << tail;
                }
                errorOut = oss.str();
                fs::remove(producedRad, ec);
                return false;
            }

            if (!fs::exists(producedRad, ec) || ec)
            {
                errorOut = "build-lod reported success but expected output is missing: " +
                           producedRad.string();
                return false;
            }

            // Move the "<stem>-lod.rad" produced by build-lod to the canonical model.rad.
            fs::remove(outputRad, ec);
            fs::rename(producedRad, outputRad, ec);
            if (ec)
            {
                // Cross-device or other rename failure: fall back to copy + remove.
                std::error_code copyEc;
                fs::copy_file(producedRad, outputRad, fs::copy_options::overwrite_existing, copyEc);
                fs::remove(producedRad, ec);
                if (copyEc)
                {
                    errorOut = "failed to move build-lod output to " + outputRad.string() +
                               ": " + copyEc.message();
                    return false;
                }
            }

            return true;
        }

    } // namespace buildlod
} // namespace ddb
