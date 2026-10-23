/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef INFO_H
#define INFO_H

#include "entry.h"
#include "output_format.h"
#include "ddb_export.h"

namespace ddb
{

    /// Describes files (and, with recursive, the folders below them) without indexing them: text
    /// (one block per file), a JSON array of entries, or a GeoJSON feature collection of those
    /// with a geometry of the kind `geometry` names ("auto", "point", "polygon")
    DDB_DLL void info(const std::vector<std::string> &input, std::ostream &output,
                      OutputFormat format = OutputFormat::Text, bool recursive = false, int maxRecursionDepth = 0, const std::string &geometry = "auto",
                      bool withHash = false, bool stopOnError = true);

    /// The same for a caller that holds the format as text (the C API, the CLI): parsed first, an
    /// unknown name throws an InvalidArgsException
    inline void info(const std::vector<std::string> &input, std::ostream &output,
                     const std::string &format, bool recursive = false, int maxRecursionDepth = 0, const std::string &geometry = "auto",
                     bool withHash = false, bool stopOnError = true)
    {
        info(input, output, parseOutputFormat(format), recursive, maxRecursionDepth, geometry, withHash, stopOnError);
    }

}

#endif // INFO_H
