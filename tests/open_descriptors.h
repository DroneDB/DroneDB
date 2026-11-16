/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef OPEN_DESCRIPTORS_H
#define OPEN_DESCRIPTORS_H

#include <system_error>

#include "fs.h"

namespace testutil
{
    // The descriptors this process holds open on `path` (the links of /proc/self/fd), or -1 where
    // the system does not list them (anything but Linux)
    inline int openDescriptorsOf(const fs::path &path)
    {
#ifdef __linux__
        const auto target = fs::weakly_canonical(path);
        int n = 0;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator("/proc/self/fd", ec))
        {
            std::error_code linkError;
            if (fs::read_symlink(e.path(), linkError) == target && !linkError)
                ++n;
        }
        return ec ? -1 : n;
#else
        (void)path;
        return -1;
#endif
    }
} // namespace testutil

#endif // OPEN_DESCRIPTORS_H
