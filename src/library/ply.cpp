/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <fstream>
#include <algorithm>
#include "logger.h"
#include "ply.h"
#include "exceptions.h"

namespace ddb
{

    namespace
    {
        // Determine whether a vertex-only PLY carries 3D Gaussian Splat attributes and,
        // if so, its spherical-harmonics degree. Operates on the property names already
        // collected in info.dimensions. A splat is never a mesh, so callers must check
        // info.isMesh first.
        void computePlySplatInfo(PlyInfo &info)
        {
            if (info.isMesh)
                return;

            const auto &dims = info.dimensions;
            const auto has = [&dims](const std::string &name) {
                return std::find(dims.begin(), dims.end(), name) != dims.end();
            };

            // Primary signature: the spherical-harmonics DC term.
            // Fallback signature: anisotropic covariance attributes (scale + rotation + opacity),
            // which a few exporters emit without an explicit f_dc_* term.
            const bool primary = has("f_dc_0");
            const bool fallback = has("scale_0") && has("scale_1") && has("scale_2") &&
                                  has("rot_0") && has("rot_1") && has("rot_2") && has("rot_3") &&
                                  has("opacity");

            if (!primary && !fallback)
                return;

            info.isSplat = true;

            // Spherical-harmonics degree from the count of f_rest_* properties.
            // Per-channel higher-order coeffs x 3 channels:
            //   0 -> degree 0, 9 -> degree 1, 24 -> degree 2, 45 -> degree 3.
            int fRestCount = 0;
            for (const auto &d : dims)
            {
                if (d.rfind("f_rest_", 0) == 0)
                    ++fRestCount;
            }

            switch (fRestCount)
            {
                case 0:  info.shDegree = 0; break;
                case 9:  info.shDegree = 1; break;
                case 24: info.shDegree = 2; break;
                case 45: info.shDegree = 3; break;
                default:
                    // Unknown/partial SH layout: clamp to the closest lower standard degree
                    // so downstream consumers still get a sane value.
                    if (fRestCount >= 45)      info.shDegree = 3;
                    else if (fRestCount >= 24) info.shDegree = 2;
                    else if (fRestCount >= 9)  info.shDegree = 1;
                    else                       info.shDegree = 0;
                    break;
            }
        }
    } // namespace

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
        info.isSplat = false;     // Assumed
        info.shDegree = -1;       // Not a splat
        info.dimensions.clear();
        info.vertexCount = 0;

        while (std::getline(in, line))
        {
            if (auto p = line.find("element vertex ") != std::string::npos)
            {
                try
                {
                    std::string vertexCountStr = line.substr(p + 14, std::string::npos);
                    // Validate format before conversion
                    for (char c : vertexCountStr)
                        if (!std::isdigit(c))
                            throw std::invalid_argument("Non-digit character in vertex count: " + vertexCountStr);

                    info.vertexCount = std::stoul(vertexCountStr);

                    // Basic validation
                    if (info.vertexCount == 0)
                        LOGW << "PLY file contains zero vertices";
                    if (info.vertexCount > 1000000000) // 1 billion vertices seems excessive
                        LOGW << "PLY file reports extremely large vertex count: " << info.vertexCount;
                }
                catch (const std::invalid_argument& e)
                {
                    LOGD << "Malformed PLY vertex count: " << line << " - " << e.what();
                    return false;
                }
                catch (const std::out_of_range& e)
                {
                    LOGD << "PLY vertex count out of range: " << line << " - " << e.what();
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
                computePlySplatInfo(info);
                return true;
            }

            // Gaussian Splat headers can be long (degree-4 spherical harmonics carry
            // 72 f_rest_* properties), so allow a generous but bounded number of lines.
            if (i++ > 512)
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
            // A 3D Gaussian Splat is a vertex-only PLY carrying SH/covariance
            // attributes. Check before falling back to a plain point cloud.
            if (info.isSplat)
            {
                return GaussianSplat;
            }
            return PointCloud;
        }
        else
            return Generic;
    }

}
