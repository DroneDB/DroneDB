/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Landlock: the process may read the folders of the policy and write in its output folders,
// and touch no other file. Needs a kernel 5.13 or later with Landlock in its list of security
// modules; every later ABI adds rights, and each is handled when the kernel knows it:
//   2  refer (link or rename between folders)      3  truncate
//   4  TCP bind and connect (no rule: none allowed) 5  ioctl on device files
//   6  scopes: no signals to, and no abstract unix sockets of, processes outside the sandbox
// The structures are declared here instead of taken from <linux/landlock.h>, which older
// headers lack in part.

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "layers.h"

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule 445
#define __NR_landlock_restrict_self 446
#endif

namespace sandbox::detail
{
    namespace
    {
        constexpr uint32_t CreateRulesetVersion = 1U << 0;
        constexpr int RulePathBeneath = 1;

        constexpr uint64_t Execute = 1ULL << 0;
        constexpr uint64_t WriteFile = 1ULL << 1;
        constexpr uint64_t ReadFile = 1ULL << 2;
        constexpr uint64_t ReadDir = 1ULL << 3;
        constexpr uint64_t RemoveDir = 1ULL << 4;
        constexpr uint64_t RemoveFile = 1ULL << 5;
        constexpr uint64_t MakeChar = 1ULL << 6;
        constexpr uint64_t MakeDir = 1ULL << 7;
        constexpr uint64_t MakeReg = 1ULL << 8;
        constexpr uint64_t MakeSock = 1ULL << 9;
        constexpr uint64_t MakeFifo = 1ULL << 10;
        constexpr uint64_t MakeBlock = 1ULL << 11;
        constexpr uint64_t MakeSym = 1ULL << 12;
        constexpr uint64_t Refer = 1ULL << 13;
        constexpr uint64_t Truncate = 1ULL << 14;
        constexpr uint64_t IoctlDev = 1ULL << 15;

        constexpr uint64_t NetBindTcp = 1ULL << 0;
        constexpr uint64_t NetConnectTcp = 1ULL << 1;

        constexpr uint64_t ScopeAbstractUnixSocket = 1ULL << 0;
        constexpr uint64_t ScopeSignal = 1ULL << 1;

        // The size of the structure grows with the ABI; a kernel refuses a size it does not know
        struct RulesetAttr
        {
            uint64_t handledAccessFs;
            uint64_t handledAccessNet;
            uint64_t scoped;
        };
        struct __attribute__((packed)) PathBeneathAttr
        {
            uint64_t allowedAccess;
            int32_t parentFd;
        };

        int abiVersion()
        {
            return static_cast<int>(syscall(__NR_landlock_create_ruleset, nullptr, 0, CreateRulesetVersion));
        }

        uint64_t handledFs(int abi)
        {
            uint64_t access = Execute | WriteFile | ReadFile | ReadDir | RemoveDir | RemoveFile | MakeChar |
                              MakeDir | MakeReg | MakeSock | MakeFifo | MakeBlock | MakeSym;
            if (abi >= 2)
                access |= Refer;
            if (abi >= 3)
                access |= Truncate;
            if (abi >= 5)
                access |= IoctlDev;
            return access;
        }

        // What a folder of the policy grants. The rights to make and remove things are for
        // folders; a single file gets the ones that apply to a file.
        constexpr uint64_t ReadRights = ReadFile | ReadDir;
        constexpr uint64_t WriteRights = ReadRights | WriteFile | RemoveDir | RemoveFile | MakeDir | MakeReg |
                                         Refer | Truncate;
        constexpr uint64_t FileRights = ReadFile | WriteFile | Truncate;

        // Allows `access` below `path`. 1 when the path is missing and optional, 0 when added,
        // -1 on failure (error says why).
        int allow(int ruleset, const std::string &path, uint64_t access, uint64_t handled, bool optional,
                  std::string *error)
        {
            const int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
            if (fd < 0)
            {
                if (errno == ENOENT && optional)
                    return 1;
                *error = "'" + path + "': " + std::strerror(errno);
                return -1;
            }
            struct stat info{};
            const bool directory = fstat(fd, &info) == 0 && S_ISDIR(info.st_mode);
            const PathBeneathAttr rule{(directory ? access : access & FileRights) & handled, fd};
            const long added = syscall(__NR_landlock_add_rule, ruleset, RulePathBeneath, &rule, 0U);
            const int failure = errno;
            close(fd);
            if (added != 0)
            {
                *error = "'" + path + "': " + std::strerror(failure);
                return -1;
            }
            return 0;
        }

        // What LeakSanitizer reads to stop the threads of the process for its leak check
        constexpr const char *LeakCheckThreads = "/proc/self/task";

        // What the rules of this build let a process read for the leak check, empty where they
        // let nothing through
        std::string leakCheckAllowance()
        {
            const SanitizerAllowances sanitizer = sanitizerAllowances();
            if (!sanitizer.leakCheck)
                return std::string();
            return std::string("; ") + LeakCheckThreads + " readable for the leak check of this " + sanitizer.runtime +
                   " build";
        }

        std::string unavailable(int failure)
        {
            if (failure == ENOSYS)
                return "the kernel has no Landlock";
            if (failure == EOPNOTSUPP)
                return "Landlock is disabled on this kernel";
            return std::string("not available: ") + std::strerror(failure);
        }
    } // namespace

    LayerState probeLandlock()
    {
        const int abi = abiVersion();
        if (abi < 1)
            return state("landlock", false, unavailable(errno));
        return state("landlock", true, "ABI " + std::to_string(abi) + leakCheckAllowance());
    }

    LayerResult applyLandlock(const Policy &policy)
    {
        const int abi = abiVersion();
        if (abi < 1)
            return result("landlock", false, unavailable(errno));

        const uint64_t handled = handledFs(abi);
        RulesetAttr attr{handled, abi >= 4 ? (NetBindTcp | NetConnectTcp) : 0,
                         abi >= 6 ? (ScopeAbstractUnixSocket | ScopeSignal) : 0};
        const size_t size = abi >= 6 ? sizeof(RulesetAttr) : abi >= 4 ? 2 * sizeof(uint64_t) : sizeof(uint64_t);
        const int ruleset = static_cast<int>(syscall(__NR_landlock_create_ruleset, &attr, size, 0U));
        if (ruleset < 0)
            return result("landlock", false, unavailable(errno));

        std::string error;
        for (const auto &path : policy.readPaths)
            if (allow(ruleset, path, ReadRights, handled, true, &error) < 0)
            {
                close(ruleset);
                return fatal("landlock", "read path " + error);
            }
        for (const auto &path : policy.writePaths)
            if (allow(ruleset, path, WriteRights, handled, false, &error) < 0)
            {
                close(ruleset);
                return fatal("landlock", "write path " + error);
            }
        // The leak check of a LeakSanitizer build lists the threads of the process
        if (sanitizerAllowances().leakCheck && allow(ruleset, LeakCheckThreads, ReadRights, handled, false, &error) < 0)
        {
            close(ruleset);
            return result("landlock", false, "the threads of the process for the leak check " + error);
        }

        // A process that could gain privileges may not restrict itself
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        {
            const int failure = errno;
            close(ruleset);
            return result("landlock", false, std::string("no_new_privs: ") + std::strerror(failure));
        }
        const long restricted = syscall(__NR_landlock_restrict_self, ruleset, 0U);
        const int failure = errno;
        close(ruleset);
        if (restricted != 0)
            return result("landlock", false, std::string("not enforced: ") + std::strerror(failure));

        std::string covers = "ABI " + std::to_string(abi) + ", files";
        if (abi >= 4)
            covers += ", TCP";
        if (abi >= 6)
            covers += ", signals and abstract sockets";
        else
            covers += "; no scoping of signals and abstract sockets before ABI 6";
        return result("landlock", true, covers + leakCheckAllowance());
    }
} // namespace sandbox::detail
