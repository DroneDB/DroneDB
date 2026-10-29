/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The POSIX platforms without seccomp and Landlock (macOS): resource limits only. The layers of
// another system are not listed: a layer is missing only where its system could offer it, so
// strict can start here and the report of a host can show every layer active.

#include "layers.h"

namespace sandbox::detail
{
    const std::vector<Layer> &platformLayers()
    {
        static const std::vector<Layer> layers = {
            {"rlimits", applyRlimits, probeRlimits},
        };
        return layers;
    }
} // namespace sandbox::detail
