/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STAC_H
#define STAC_H

#include <string>
#include "dbops.h"
#include <vector>

namespace ddb
{

    json generateStac(const std::string &ddbPath,
                      const std::string &entry = "",
                      const std::string &stacCollectionRoot = ".",
                      const std::string &id = "",
                      const std::string &stacCatalogRoot = "");

    // The filters and the page of a STAC ItemCollection
    struct StacItemQuery
    {
        std::vector<double> bbox{};   // [minX, minY, maxX, maxY] in EPSG:4326; anything else = no filter
        std::string datetimeStart{};  // RFC 3339; empty = open
        std::string datetimeEnd{};
        int limit = 10;               // a value of 0 or less is 10
        int offset = 0;
    };

    // The query of a STAC API request, from its parameters as the request has them (text): bbox
    // "minX,minY,maxX,maxY" (EPSG:4326), datetime a single instant or an interval "start/end"
    // (".." or an empty bound is open), both may be empty (no filter). Throws
    // InvalidArgsException for a bbox that is not four numbers.
    StacItemQuery parseStacItemQuery(const std::string &bbox, const std::string &datetime, int limit, int offset);

    // Generate a STAC API ItemCollection (GeoJSON FeatureCollection of STAC Items)
    // for all geometry-bearing entries of a dataset, with optional bbox / datetime
    // filtering and limit/offset paging.
    json generateStacItemCollection(const std::string &ddbPath,
                                    const std::string &stacCollectionRoot = ".",
                                    const std::string &id = "",
                                    const std::string &stacCatalogRoot = "",
                                    const StacItemQuery &query = {});

    // Data-only STAC (XI-09): the documents of generateStac (with id "") and
    // generateStacItemCollection without any member built from a URL or an ID: no "id",
    // "collection" or "links", and no asset "href". Everything else is the same. Throws
    // EntryNotFoundException for an entry that is not in the index.
    json generateStacData(const std::string& ddbPath, const std::string& entry = "");

    json generateStacItemCollectionData(const std::string& ddbPath, const StacItemQuery& query = {});
}

#endif // STAC_H
