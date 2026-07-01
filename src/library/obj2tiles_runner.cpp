/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "obj2tiles_runner.h"

#include <cstdlib>
#include <iomanip>
#include <locale>
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
    namespace obj2tiles
    {

        namespace
        {

#ifdef _WIN32
            constexpr const char *kBinaryName = "Obj2Tiles.exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kBinaryName = "Obj2Tiles";
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

            // Format a double with the classic (period) locale and enough precision
            // for geographic coordinates. Obj2Tiles is built with InvariantGlobalization,
            // so it parses CLI doubles with a period decimal separator regardless of the
            // host locale; we must therefore never emit a locale-specific separator.
            std::string fmtDouble(double v)
            {
                std::ostringstream oss;
                oss.imbue(std::locale::classic());
                oss << std::setprecision(15) << v;
                return oss.str();
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

            // Append the CLI arguments derived from opts (everything after the input
            // and output positional arguments).
            void appendOptionArgs(std::vector<std::string> &args, const Obj2TilesOptions &opts)
            {
                args.push_back("--divisions");
                args.push_back(std::to_string(opts.divisions));
                args.push_back("--lods");
                args.push_back(std::to_string(opts.lods));

                if (opts.localMode)
                {
                    args.push_back("--local");
                }
                else
                {
                    if (opts.lat.has_value())
                    {
                        args.push_back("--lat");
                        args.push_back(fmtDouble(*opts.lat));
                    }
                    if (opts.lon.has_value())
                    {
                        args.push_back("--lon");
                        args.push_back(fmtDouble(*opts.lon));
                    }
                    args.push_back("--alt");
                    args.push_back(fmtDouble(opts.alt));
                }

                // Fork-only flags (OpenDroneMap/Obj2Tiles#95): emit only when set to a
                // non-default value so the same code path keeps working with the
                // upstream v1.4.0 binary, which does not understand them.
                if (opts.octree)
                    args.push_back("--octree");
                if (opts.lodTextureScale != 1.0)
                {
                    args.push_back("--lod-texture-scale");
                    args.push_back(fmtDouble(opts.lodTextureScale));
                }
            }

            // Keep only the tail of the captured child output for inclusion in error
            // messages (the full output is always logged at debug level).
            std::string tail(const std::string &s, size_t maxLen = 600)
            {
                if (s.size() <= maxLen) return s;
                return "..." + s.substr(s.size() - maxLen);
            }

        } // namespace

        fs::path findObj2TilesBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var. When DDB_OBJ2TILES_PATH is set
            // (non-empty), it is authoritative: either it points to a usable binary
            // or no binary is considered available (so callers can pin the tool
            // deterministically and tests can simulate "missing").
            if (const char *override = std::getenv("DDB_OBJ2TILES_PATH"))
            {
                if (override[0] != '\0')
                {
                    fs::path p(override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "Obj2Tiles binary discovered via DDB_OBJ2TILES_PATH at " << found.string();
                    else
                        LOGD << "DDB_OBJ2TILES_PATH=" << override << " is not an executable file; treating Obj2Tiles as unavailable";
                    return found;
                }
            }

            // 2. Folder of the current executable (where Obj2Tiles is bundled by build/packaging)
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty())
                {
                    fs::path candidate = exeDir / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 3. <install_prefix>/bin (parent of exe folder + /bin) - covers cmake --install layouts
            if (found.empty())
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty() && exeDir.has_parent_path())
                {
                    fs::path candidate = exeDir.parent_path() / "bin" / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 4. System PATH
            if (found.empty()) found = searchOnPath();

            g_cachedBinary = found;
            g_cacheValid = true;

            if (!found.empty())
                LOGD << "Obj2Tiles binary discovered at " << found.string();
            else
                LOGD << "Obj2Tiles binary not found in DDB_OBJ2TILES_PATH, exe folder, install prefix or PATH";

            return found;
        }

        bool runObj2Tiles(const fs::path &inputObj,
                           const fs::path &outDir,
                           const Obj2TilesOptions &opts,
                           std::string &errorOut,
                           const fs::path &binary)
        {
            errorOut.clear();

            fs::path bin = binary.empty() ? findObj2TilesBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                errorOut = "Obj2Tiles binary not found";
                return false;
            }

            std::error_code ec;
            if (!fs::exists(inputObj, ec))
            {
                errorOut = "input OBJ does not exist: " + inputObj.string();
                return false;
            }

            // Make sure the output directory exists (Obj2Tiles also creates it, but we
            // want a clean, existing target before launching).
            fs::create_directories(outDir, ec);
            if (ec)
            {
                errorOut = "could not create output directory '" + outDir.string() +
                           "': " + ec.message();
                return false;
            }

            std::vector<std::string> args;
            args.push_back(bin.string());
            args.push_back(inputObj.string()); // positional: Input
            args.push_back(outDir.string());   // positional: Output
            appendOptionArgs(args, opts);

            // Log full command line for debugging.
            {
                std::ostringstream cmdDbg;
                for (size_t i = 0; i < args.size(); ++i)
                {
                    if (i > 0) cmdDbg << ' ';
                    cmdDbg << quoteArg(args[i]);
                }
                LOGD << "Spawning Obj2Tiles: " << cmdDbg.str();
            }

            std::string childOutput;

#ifdef _WIN32
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;

            HANDLE hOutRead = nullptr, hOutWrite = nullptr;
            if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0))
            {
                errorOut = "CreatePipe failed for Obj2Tiles output";
                return false;
            }
            // The read end must NOT be inherited by the child.
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
                wideBin.c_str(), // lpApplicationName: explicit resolved path
                wideCmd.data(),  // lpCommandLine: full quoted command line for argv
                nullptr, nullptr,
                TRUE, // bInheritHandles: child inherits hOutWrite
                CREATE_NO_WINDOW,
                nullptr, nullptr,
                &si, &pi);

            // Close write end in parent immediately after spawning so ReadFile reaches EOF.
            CloseHandle(hOutWrite);

            if (!ok)
            {
                CloseHandle(hOutRead);
                DWORD err = GetLastError();
                std::ostringstream oss;
                oss << "CreateProcess failed for Obj2Tiles (error " << err << ")";
                errorOut = oss.str();
                return false;
            }

            // Drain the child's combined stdout/stderr until EOF (child exit).
            {
                char buf[4096];
                DWORD nr = 0;
                while (ReadFile(hOutRead, buf, sizeof(buf), &nr, nullptr) && nr > 0)
                    childOutput.append(buf, nr);
            }
            CloseHandle(hOutRead);

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            if (!childOutput.empty())
                LOGD << "[Obj2Tiles] " << childOutput;

            if (exitCode != 0)
            {
                std::ostringstream oss;
                oss << "Obj2Tiles exited with code " << exitCode;
                if (!childOutput.empty()) oss << ": " << tail(childOutput);
                errorOut = oss.str();
                return false;
            }
#else
            int outPipe[2];
            if (pipe(outPipe) != 0)
            {
                errorOut = "pipe() failed for Obj2Tiles output";
                return false;
            }

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            // Redirect the child's stdout and stderr to the pipe write end, then close
            // both original pipe descriptors inside the child.
            posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, outPipe[0]);
            posix_spawn_file_actions_addclose(&actions, outPipe[1]);

            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);

            pid_t pid = 0;
            int spawnRet = posix_spawn(&pid, bin.c_str(), &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);

            // Close the write end in the parent so read() below reaches EOF when the child exits.
            close(outPipe[1]);

            if (spawnRet != 0)
            {
                close(outPipe[0]);
                std::ostringstream oss;
                oss << "posix_spawn failed for Obj2Tiles (errno " << spawnRet << ")";
                errorOut = oss.str();
                return false;
            }

            // Drain the child's combined stdout/stderr until EOF (child exit).
            {
                char buf[4096];
                ssize_t nr = 0;
                while ((nr = read(outPipe[0], buf, sizeof(buf))) > 0)
                    childOutput.append(buf, static_cast<size_t>(nr));
            }
            close(outPipe[0]);

            int status = 0;
            if (waitpid(pid, &status, 0) < 0)
            {
                errorOut = "waitpid failed for Obj2Tiles";
                return false;
            }

            if (!childOutput.empty())
                LOGD << "[Obj2Tiles] " << childOutput;

            if (!WIFEXITED(status))
            {
                errorOut = "Obj2Tiles terminated abnormally";
                return false;
            }
            int exitCode = WEXITSTATUS(status);
            if (exitCode != 0)
            {
                std::ostringstream oss;
                oss << "Obj2Tiles exited with code " << exitCode;
                if (!childOutput.empty()) oss << ": " << tail(childOutput);
                errorOut = oss.str();
                return false;
            }
#endif

            const fs::path tilesetPath = outDir / "tileset.json";
            if (!fs::exists(tilesetPath, ec))
            {
                errorOut = "Obj2Tiles reported success but tileset.json is missing: " + tilesetPath.string();
                return false;
            }

            return true;
        }

    } // namespace obj2tiles
} // namespace ddb
