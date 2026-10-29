/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The POSIX platforms without seccomp and Landlock (macOS): resource limits only. The two
// confinement layers are listed as missing, so that best-effort says so once and strict
// refuses.

#include "layers.h"

namespace sandbox::detail
{
    namespace
    {
        constexpr const char *LinuxOnly = "only available on Linux";

        LayerResult applySeccomp(const Policy &)
        {
            return result("seccomp", false, LinuxOnly);
        }
        LayerState probeSeccomp()
        {
            return unsupportedProbe("seccomp", LinuxOnly);
        }
        LayerResult applyLandlock(const Policy &)
        {
            return result("landlock", false, LinuxOnly);
        }
        LayerState probeLandlock()
        {
            return unsupportedProbe("landlock", LinuxOnly);
        }
    } // namespace

    const std::vector<Layer> &platformLayers()
    {
        static const std::vector<Layer> layers = {
            {"rlimits", applyRlimits, probeRlimits},
            {"seccomp", applySeccomp, probeSeccomp},
            {"landlock", applyLandlock, probeLandlock},
        };
        return layers;
    }
} // namespace sandbox::detail
