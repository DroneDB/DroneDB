/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_types.h"

namespace ddb{

std::string typeToHuman(EntryType t){
    switch(t){
        case EntryType::Undefined:
            return "Undefined";
        case EntryType::Directory:
            return "Directory";
        case EntryType::Generic:
            return "Generic";
        case EntryType::GeoImage:
            return "GeoImage";
        case EntryType::GeoRaster:
            return "GeoRaster";
        case EntryType::PointCloud:
            return "PointCloud";
        case EntryType::Image:
            return "Image";
        case EntryType::DroneDB:
            return "DroneDB";
        default:
            return "?";
    }
}

}

