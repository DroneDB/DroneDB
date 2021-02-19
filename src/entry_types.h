/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRYTYPES_H
#define ENTRYTYPES_H

#include <string>
#include <map>
#include "ddb_export.h"

namespace ddb{

enum EntryType { 
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
    GeoVideo = 10
};

DDB_DLL std::string typeToHuman(EntryType t);

}

#endif // ENTRYTYPES_H
