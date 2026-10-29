/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Resource limits of the POSIX platforms: a parser that loops, allocates without end or writes
// a huge file ends instead of taking the machine with it.

#include <sys/resource.h>

#include <cerrno>
#include <cstring>

#include "layers.h"

namespace sandbox::detail
{
    namespace
    {
        // Sets soft and hard limit of `resource` to `value`, or as near as the hard limit
        // already in force allows (it can only be lowered). 0 leaves the limit alone.
        bool limit(int resource, uint64_t value, uint64_t hardGap, std::string *error)
        {
            if (value == 0)
                return true;
            rlimit current{};
            if (getrlimit(resource, &current) != 0)
            {
                *error = std::strerror(errno);
                return false;
            }
            rlim_t soft = static_cast<rlim_t>(value);
            // Where the hard limit is the one that ends the process (RLIMIT_CPU: SIGXCPU at the
            // soft limit, SIGKILL at the hard one) it lies a little further
            rlim_t hard = static_cast<rlim_t>(value + hardGap);
            if (current.rlim_max != RLIM_INFINITY)
            {
                if (hard > current.rlim_max)
                    hard = current.rlim_max;
                if (soft > hard)
                    soft = hard;
            }
            const rlimit wanted{soft, hard};
            if (setrlimit(resource, &wanted) != 0)
            {
                *error = std::strerror(errno);
                return false;
            }
            return true;
        }
    } // namespace

    LayerResult applyRlimits(const Policy &policy)
    {
        std::string error;
        bool asEnforced = true;
        if (!limit(RLIMIT_CPU, policy.maxCpuSeconds, 1, &error) ||
            !limit(RLIMIT_FSIZE, policy.maxFileBytes, 0, &error) ||
            !limit(RLIMIT_NOFILE, policy.maxOpenFiles, 0, &error))
            return result("rlimits", false, error);

        if (policy.maxMemoryBytes != 0)
        {
#ifdef __APPLE__
            // macOS accepts RLIMIT_AS and does not enforce it; a failure to set it is no loss
            asEnforced = false;
            std::string ignored;
            limit(RLIMIT_AS, policy.maxMemoryBytes, 0, &ignored);
#else
            if (!limit(RLIMIT_AS, policy.maxMemoryBytes, 0, &error))
                return result("rlimits", false, error);
#endif
        }

        // No core file: it would hold the memory of the user's file
        const rlimit noCore{0, 0};
        if (setrlimit(RLIMIT_CORE, &noCore) != 0)
            return result("rlimits", false, std::strerror(errno));

        return result("rlimits", true, asEnforced ? "address space, CPU time, file size, open files, core files"
                                                  : "CPU time, file size, open files, core files; the address "
                                                    "space limit is not enforced by this OS");
    }

    LayerState probeRlimits()
    {
        return state("rlimits", true);
    }
} // namespace sandbox::detail
