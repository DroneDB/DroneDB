/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STAC_H
#define STAC_H

#include <string>
#include "ddb_export.h"
#include "dbops.h"
#include <vector>

namespace ddb
{

    DDB_DLL json generateStac(const std::string &ddbPath,
                              const std::string &entry = "",
                              const std::string &stacCollectionRoot = ".",
                              const std::string &id = "",
                              const std::string &stacCatalogRoot = "");

    // Generate a STAC API ItemCollection (GeoJSON FeatureCollection of STAC Items)
    // for all geometry-bearing entries of a dataset, with optional bbox / datetime
    // filtering and limit/offset paging.
    DDB_DLL json generateStacItemCollection(const std::string &ddbPath,
                                            const std::string &stacCollectionRoot = ".",
                                            const std::string &id = "",
                                            const std::string &stacCatalogRoot = "",
                                            const std::vector<double> &bbox = {},
                                            const std::string &datetimeStart = "",
                                            const std::string &datetimeEnd = "",
                                            int limit = 10,
                                            int offset = 0);

    // Data-only STAC (XI-09): the documents of generateStac (with id "") and
    // generateStacItemCollection without any member built from a URL or an ID: no "id",
    // "collection" or "links", and no asset "href". Everything else is the same. Throws
    // EntryNotFoundException for an entry that is not in the index.
    DDB_DLL json generateStacData(const std::string& ddbPath, const std::string& entry = "");

    DDB_DLL json generateStacItemCollectionData(const std::string& ddbPath,
                                                const std::vector<double>& bbox = {},
                                                const std::string& datetimeStart = "",
                                                const std::string& datetimeEnd = "",
                                                int limit = 10,
                                                int offset = 0);
}

#endif // STAC_H
