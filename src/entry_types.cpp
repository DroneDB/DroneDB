/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_types.h"

namespace ddb {

std::map<EntryType, std::string> typeMapper {
    { EntryType::Undefined, "Undefined"},
    { EntryType::Directory, "Directory"},
    { EntryType::Generic, "Generic"},
    { EntryType::GeoImage, "GeoImage"},
    { EntryType::GeoRaster, "GeoRaster"},
    { EntryType::PointCloud, "PointCloud"},
    { EntryType::Image, "Image"},
    { EntryType::DroneDB, "DroneDB"},
    { EntryType::Markdown, "Markdown"},
    { EntryType::Video, "Video"},
    { EntryType::GeoVideo, "GeoVideo"}
};

std::string typeToHuman(EntryType t){
    const auto res = typeMapper.find(t);

    return res != typeMapper.end() ? res->second : "?";
}

}

