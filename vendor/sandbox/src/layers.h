/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Private: the layers of a platform, listed by the file of that platform and run by apply().

#ifndef SANDBOX_LAYERS_H
#define SANDBOX_LAYERS_H

#include <string>
#include <utility>
#include <vector>

#include "sandbox/sandbox.h"

namespace sandbox::detail
{
    struct LayerResult
    {
        LayerState state;
        // The policy itself cannot be honoured (a write path that is missing), whatever the host
        // offers: the process must not go on in any mode
        bool fatal = false;
    };

    struct Layer
    {
        const char *name;
        LayerResult (*apply)(const Policy &policy);
        // Whether the layer can be applied here, without applying it
        LayerState (*probe)();
    };

    // In the order they are applied
    const std::vector<Layer> &platformLayers();

    inline LayerState state(const char *name, bool active, std::string detail = std::string())
    {
        return LayerState{name, active, std::move(detail)};
    }
    inline LayerResult result(const char *name, bool active, std::string detail = std::string())
    {
        return LayerResult{state(name, active, std::move(detail)), false};
    }
    inline LayerResult fatal(const char *name, std::string detail)
    {
        return LayerResult{state(name, false, std::move(detail)), true};
    }

#ifndef _WIN32
    // rlimits (rlimits.cpp), shared by the POSIX platforms
    LayerResult applyRlimits(const Policy &policy);
    LayerState probeRlimits();
#endif

#ifdef __linux__
    // The Linux layers, one file each (seccomp_linux.cpp, landlock_linux.cpp)
    LayerResult applySeccomp(const Policy &policy);
    LayerState probeSeccomp();
    LayerResult applyLandlock(const Policy &policy);
    LayerState probeLandlock();
#endif
} // namespace sandbox::detail

#endif
