/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "untwine_runner.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "exceptions.h"
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
    namespace untwine
    {

        namespace
        {

#ifdef _WIN32
            constexpr const char *kBinaryName = "untwine.exe";
            constexpr char kPathSep = ';';
#else
            constexpr const char *kBinaryName = "untwine";
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
                // Check executable bit for current user/group/other
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
                // Quote arguments containing spaces or quotes for CommandLineToArgvW round-trip.
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
                        // Escape any preceding backslashes plus the quote itself.
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
                // Trailing backslashes need to be doubled before the closing quote.
                out.append(backslashes, '\\');
                out.push_back('"');
                return out;
#else
                return arg;
#endif
            }

        } // namespace

        fs::path findUntwineBinary(bool forceRefresh)
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (!forceRefresh && g_cacheValid) return g_cachedBinary;

            fs::path found;

            // 1. Explicit override via env var. When DDB_UNTWINE_PATH is set
            // (non-empty), it is authoritative: either it points to a usable
            // binary or no binary is considered available (so callers can pin
            // the backend deterministically and tests can simulate "missing").
            if (const char *override = std::getenv("DDB_UNTWINE_PATH"))
            {
                if (override[0] != '\0')
                {
                    fs::path p(override);
                    if (isExecutableFile(p)) found = p;
                    g_cachedBinary = found;
                    g_cacheValid = true;
                    if (!found.empty())
                        LOGD << "Untwine binary discovered via DDB_UNTWINE_PATH at " << found.string();
                    else
                        LOGD << "DDB_UNTWINE_PATH=" << override << " is not an executable file; treating Untwine as unavailable";
                    return found;
                }
            }

            // 2. Folder of the current executable (where untwine is bundled by build/packaging)
            {
                fs::path exeDir = io::getExeFolderPath();
                if (!exeDir.empty())
                {
                    fs::path candidate = exeDir / kBinaryName;
                    if (isExecutableFile(candidate)) found = candidate;
                }
            }

            // 3. <install_prefix>/bin (parent of exe folder + /bin) — covers cmake --install layouts
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
                LOGD << "Untwine binary discovered at " << found.string();
            else
                LOGD << "Untwine binary not found in DDB_UNTWINE_PATH, exe folder, install prefix or PATH";

            return found;
        }

        CopcBackend resolveBackend(CopcBackend requested)
        {
            // Hard override: legacy switch
            if (const char *forcePdal = std::getenv("DDB_USE_PDAL_COPC"))
            {
                if (forcePdal[0] == '1')
                {
                    LOGD << "DDB_USE_PDAL_COPC=1 forces PDAL COPC backend";
                    return CopcBackend::Pdal;
                }
            }

            // Explicit backend env (overrides argument)
            if (const char *envBackend = std::getenv("DDB_COPC_BACKEND"))
            {
                std::string s(envBackend);
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                if (s == "untwine") requested = CopcBackend::Untwine;
                else if (s == "pdal") requested = CopcBackend::Pdal;
                else if (s == "auto") requested = CopcBackend::Auto;
                // unknown values are silently ignored
            }

            if (requested == CopcBackend::Pdal) return CopcBackend::Pdal;

            fs::path bin = findUntwineBinary();

            if (requested == CopcBackend::Untwine)
            {
                if (bin.empty())
                    throw UntwineException("Untwine backend requested but no untwine binary could be located. "
                                           "Set DDB_UNTWINE_PATH or place untwine next to the DroneDB executable.");
                return CopcBackend::Untwine;
            }

            // Auto
            return bin.empty() ? CopcBackend::Pdal : CopcBackend::Untwine;
        }

        bool runUntwine(const std::vector<std::string> &inputFiles,
                        const fs::path &outputFile,
                        const fs::path &tempDir,
                        std::string &errorOut,
                        const fs::path &binary)
        {
            errorOut.clear();

            fs::path bin = binary.empty() ? findUntwineBinary() : binary;
            if (bin.empty() || !isExecutableFile(bin))
            {
                errorOut = "untwine binary not found";
                return false;
            }

            if (inputFiles.empty())
            {
                errorOut = "no input files provided";
                return false;
            }

            // Build comma-separated file list (Untwine accepts a comma-separated --files value).
            std::string filesArg;
            for (size_t i = 0; i < inputFiles.size(); ++i)
            {
                if (i > 0) filesArg.push_back(',');
                filesArg.append(inputFiles[i]);
            }

            // Make sure output dir exists; remove any pre-existing output file so untwine has a clean slate.
            std::error_code ec;
            fs::create_directories(outputFile.parent_path(), ec);
            if (fs::exists(outputFile, ec)) fs::remove(outputFile, ec);

            std::vector<std::string> args;
            args.push_back(bin.string());
            args.push_back("--files");
            args.push_back(filesArg);
            args.push_back("--output_dir");
            args.push_back(outputFile.string());
            if (!tempDir.empty())
            {
                fs::create_directories(tempDir, ec);
                args.push_back("--temp_dir");
                args.push_back(tempDir.string());
            }

            LOGD << "Spawning untwine: " << bin.string() << " with " << inputFiles.size() << " input file(s)";

#ifdef _WIN32
            // Build a single command line string
            std::string cmdLine;
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (i > 0) cmdLine.push_back(' ');
                cmdLine.append(quoteArg(args[i]));
            }

            // Convert to wide
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, nullptr, 0);
            std::wstring wideCmd(static_cast<size_t>(wideLen > 0 ? wideLen : 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, wideCmd.data(), wideLen);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};

            BOOL ok = CreateProcessW(
                nullptr,
                wideCmd.data(),
                nullptr, nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr, nullptr,
                &si, &pi);

            if (!ok)
            {
                DWORD err = GetLastError();
                std::ostringstream oss;
                oss << "CreateProcess failed for untwine (error " << err << ")";
                errorOut = oss.str();
                return false;
            }

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            if (exitCode != 0)
            {
                std::ostringstream oss;
                oss << "untwine exited with code " << exitCode;
                errorOut = oss.str();
                return false;
            }
#else
            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);

            pid_t pid = 0;
            int spawnRet = posix_spawn(&pid, bin.c_str(), nullptr, nullptr, argv.data(), environ);
            if (spawnRet != 0)
            {
                std::ostringstream oss;
                oss << "posix_spawn failed for untwine (errno " << spawnRet << ")";
                errorOut = oss.str();
                return false;
            }

            int status = 0;
            if (waitpid(pid, &status, 0) < 0)
            {
                errorOut = "waitpid failed for untwine";
                return false;
            }

            if (!WIFEXITED(status))
            {
                errorOut = "untwine terminated abnormally";
                return false;
            }
            int exitCode = WEXITSTATUS(status);
            if (exitCode != 0)
            {
                std::ostringstream oss;
                oss << "untwine exited with code " << exitCode;
                errorOut = oss.str();
                return false;
            }
#endif

            if (!fs::exists(outputFile))
            {
                errorOut = "untwine reported success but output file is missing: " + outputFile.string();
                return false;
            }

            return true;
        }

    } // namespace untwine
} // namespace ddb
