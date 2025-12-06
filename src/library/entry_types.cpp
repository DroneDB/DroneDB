/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "entry_types.h"
#include <algorithm>
#include <vector>

namespace ddb
{

    std::map<EntryType, std::string> typeMapper{
        {EntryType::Undefined, "Undefined"},
        {EntryType::Directory, "Directory"},
        {EntryType::Generic, "Generic"},
        {EntryType::GeoImage, "GeoImage"},
        {EntryType::GeoRaster, "GeoRaster"},
        {EntryType::PointCloud, "PointCloud"},
        {EntryType::Image, "Image"},
        {EntryType::DroneDB, "DroneDB"},
        {EntryType::Markdown, "Markdown"},
        {EntryType::Video, "Video"},
        {EntryType::GeoVideo, "GeoVideo"},
        {EntryType::Model, "Model"},
        {EntryType::Panorama, "Panorama"},
        {EntryType::GeoPanorama, "GeoPanorama"},
        {EntryType::Vector, "Vector"}};

    std::string typeToHuman(EntryType t)
    {
        const auto res = typeMapper.find(t);

        return res != typeMapper.end() ? res->second : "?";
    }

    EntryType typeFromHuman(const std::string &s)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        for (const auto &pair : typeMapper)
        {
            std::string typeName = pair.second;
            std::transform(typeName.begin(), typeName.end(), typeName.begin(), ::tolower);
            if (typeName == lower)
                return pair.first;
        }
        return EntryType::Undefined;
    }

    std::vector<std::string> getEntryTypeNames()
    {
        std::vector<std::string> names;
        for (const auto &pair : typeMapper)
        {
            // Skip Directory and Undefined as they cannot be rescanned
            if (pair.first != EntryType::Directory && pair.first != EntryType::Undefined)
            {
                std::string name = pair.second;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                names.push_back(name);
            }
        }
        return names;
    }

}
