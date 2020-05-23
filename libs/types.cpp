/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "types.h"

namespace entry{

std::string typeToHuman(Type t){
    switch(t){
        case Type::Undefined:
            return "Undefined";
        case Type::Directory:
            return "Directory";
        case Type::Generic:
            return "Generic";
        case Type::GeoImage:
            return "GeoImage";
        case Type::GeoRaster:
            return "GeoRaster";
        case Type::PointCloud:
            return "PointCloud";
        case Type::Image:
            return "Image";
        case Type::DroneDB:
            return "DroneDB";
        default:
            return "?";
    }
}

}

