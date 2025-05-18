/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <fstream>
#include "logger.h"
#include "ply.h"
#include "exceptions.h"

namespace ddb
{

    DDB_DLL bool getPlyInfo(const fs::path &plyFile, PlyInfo &info)
    {
        std::ifstream in(plyFile.string());
        if (!in.is_open())
            throw FSException("Cannot open " + plyFile.string());

        int i = 0;
        std::string line;

        if (!std::getline(in, line))
            return false;
        if (line != "ply")
            return false;

        info.isMesh = false;      // Assumed
        info.hasTextures = false; // Assumed
        info.dimensions.clear();
        info.vertexCount = 0;

        while (std::getline(in, line))
        {

            if (auto p = line.find("element vertex ") != std::string::npos)
            {
                try
                {
                    info.vertexCount = std::stoul(line.substr(p + 14, std::string::npos));
                }
                catch (std::invalid_argument)
                {
                    LOGD << "Malformed PLY: " << line;
                    return false;
                }
            }
            else if (auto p = line.find("property ") != std::string::npos)
            {
                auto propNamePos = line.rfind(" ");
                if (propNamePos != std::string::npos)
                {
                    auto propName = line.substr(propNamePos + 1, std::string::npos);
                    info.dimensions.push_back(propName);
                }
            }
            else if (auto p = line.find("comment TextureFile ") != std::string::npos)
            {
                info.hasTextures = true;
            }
            else if (auto p = line.find("element face ") != std::string::npos)
            {
                info.isMesh = true;
            }
            else if (line == "end_header")
            {
                return true;
            }

            if (i++ > 100)
            {
                LOGD << "Hit PLY parser limit";
                break; // Limit
            }
        }

        return false;
    }

    EntryType identifyPly(const fs::path &plyFile)
    {
        PlyInfo info;
        if (getPlyInfo(plyFile, info))
        {
            if (info.isMesh)
            {
                // We do not support textured PLY models
                // (nexus has trouble building some of them?)
                return info.hasTextures ? Generic : Model;
            }
            else
            {
                return PointCloud;
            }
        }
        else
            return Generic;
    }

}
