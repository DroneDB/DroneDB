/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRYTYPES_H
#define ENTRYTYPES_H

#include <string>
#include <map>
#include <vector>
#include "ddb_export.h"

namespace ddb
{

    // The kind of an indexed entry. The numeric values are stored in the index (entries.type) and
    // returned to hosts as integers (the C API, Registry): they never change, and a new type takes
    // the next number.
    enum class EntryType : int
    {
        Undefined = 0,
        Directory = 1,
        Generic = 2,
        GeoImage = 3,
        GeoRaster = 4,
        PointCloud = 5,
        Image = 6,
        DroneDB = 7,
        Markdown = 8,
        Video = 9,
        GeoVideo = 10,
        Model = 11,
        Panorama = 12,
        GeoPanorama = 13,
        Vector = 14,
        GaussianSplat = 15,
        Tiles3D = 16
    };

    // The number of a type, as stored in the index and returned to hosts
    constexpr int entryTypeValue(EntryType t) noexcept { return static_cast<int>(t); }

    DDB_DLL std::string typeToHuman(EntryType t);
    DDB_DLL EntryType typeFromHuman(const std::string &s);
    DDB_DLL std::vector<std::string> getEntryTypeNames();

}

#endif // ENTRYTYPES_H
