/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TYPES_H
#define TYPES_H

#include <string>

namespace entry{

enum Type { Undefined = 0, Directory = 1, Generic, GeoImage, GeoRaster, PointCloud, Image, DroneDB };

std::string typeToHuman(Type t);

}

#endif // TYPES_H
