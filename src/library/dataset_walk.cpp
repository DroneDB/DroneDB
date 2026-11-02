/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dataset_walk.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h> // DWORD, GetFileAttributesW
#endif

#include "entry_path.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb
{

    bool isHiddenOrSystem(const fs::path &path)
    {
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
        (void)path;
        return false;
#endif
    }

    bool hasIndexableName(const fs::path &path)
    {
        if (!path.has_filename())
            return true;
        const auto name = io::toGenericUtf8(path.filename());
        return std::none_of(name.begin(), name.end(), isEntryPathSeparator);
    }

    void walkDataset(const fs::path &folder, const WalkOptions &options,
                     const std::function<void(const fs::directory_entry &entry)> &visit)
    {
        try
        {
            for (auto i = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied);
                 i != fs::recursive_directory_iterator(); ++i)
            {
                const fs::path &path = i->path();

                if (isReservedSegment(path.filename()) || isHiddenOrSystem(path))
                {
                    i.disable_recursion_pending();
                    continue;
                }

                if (options.levels > 0 && i.depth() >= options.levels - 1)
                    i.disable_recursion_pending();

                visit(*i);
            }
        }
        catch (const fs::filesystem_error &e)
        {
            throw FSException(e.what());
        }
    }

} // namespace ddb
