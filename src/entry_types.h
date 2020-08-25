/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRYTYPES_H
#define ENTRYTYPES_H

#include <string>
#include "ddb_export.h"

namespace ddb{

enum EntryType { Undefined = 0, Directory = 1, Generic, GeoImage, GeoRaster, PointCloud, Image, DroneDB };

DDB_DLL std::string typeToHuman(EntryType t);

}

#endif // ENTRYTYPES_H
