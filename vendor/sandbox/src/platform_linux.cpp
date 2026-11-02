/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The Linux layers in the order they are applied. The ones that need /proc go before Landlock
// takes the file system away.

#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "layers.h"

namespace sandbox::detail
{
    namespace
    {
        // The out-of-memory killer picks the highest score: the helper is the process to lose
        constexpr const char *OomScoreFile = "/proc/self/oom_score_adj";

        LayerResult applyOomScore(const Policy &)
        {
            const int fd = open(OomScoreFile, O_WRONLY | O_CLOEXEC);
            if (fd < 0)
                return result("oom-score", false, std::string(OomScoreFile) + ": " + std::strerror(errno));
            const char value[] = "1000\n";
            const ssize_t written = write(fd, value, sizeof(value) - 1);
            const int failure = errno;
            close(fd);
            if (written != static_cast<ssize_t>(sizeof(value) - 1))
                return result("oom-score", false, std::string(OomScoreFile) + ": " + std::strerror(failure));
            return result("oom-score", true, "oom_score_adj 1000");
        }

        LayerState probeOomScore()
        {
            if (access(OomScoreFile, W_OK) != 0)
                return state("oom-score", false, std::string(OomScoreFile) + ": " + std::strerror(errno));
            return state("oom-score", true);
        }

        LayerResult applyNoNewPrivs(const Policy &)
        {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
                return result("no-new-privs", false, std::strerror(errno));
            return result("no-new-privs", true, "set-uid and file capabilities have no effect");
        }

        LayerState probeNoNewPrivs()
        {
            if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) < 0)
                return state("no-new-privs", false, std::strerror(errno));
            return state("no-new-privs", true);
        }
    } // namespace

    const std::vector<Layer> &platformLayers()
    {
        static const std::vector<Layer> layers = {
            {"rlimits", applyRlimits, probeRlimits},
            {"oom-score", applyOomScore, probeOomScore},
            {"no-new-privs", applyNoNewPrivs, probeNoNewPrivs},
            {"landlock", applyLandlock, probeLandlock},
            {"seccomp", applySeccomp, probeSeccomp},
        };
        return layers;
    }
} // namespace sandbox::detail
