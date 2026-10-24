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

    /// How `info` describes files
    struct InfoOptions
    {
        OutputFormat format = OutputFormat::Text;
        bool recursive = false;         ///< also the folders below the paths
        int maxRecursionDepth = 0;
        std::string geometry = "auto";  ///< GeoJSON geometry of an entry: "auto", "point" or "polygon"
        bool withHash = false;
        bool stopOnError = true;        ///< a file that cannot be parsed ends the call (else it is skipped)
    };

    /// Describes files (and, with options.recursive, the folders below them) without indexing
    /// them: text (one block per file), a JSON array of entries, or a GeoJSON feature collection
    /// of those
    DDB_DLL void info(const std::vector<std::string> &input, std::ostream &output,
                      const InfoOptions &options = {});

}

#endif // INFO_H
