/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef _3D_H
#define _3D_H

#include <string>
#include "ddb_export.h"
#ifndef NO_NEXUS
#include <nxs.h>
#endif
#include <vector>

namespace ddb
{

    DDB_DLL std::string buildNexus(const std::string &inputObj, const std::string &outputNxs, bool overwrite = false);
    DDB_DLL std::vector<std::string> getObjDependencies(const std::string &obj);
    DDB_DLL std::vector<std::string> getGltfDependencies(const std::string &gltf);

    /// Convert glTF/GLB to OBJ or PLY format
    /// @param inputGltf Path to the input glTF or GLB file
    /// @param outputBasePath Base path for output files (without extension)
    /// @param outGeomPath Output parameter for the generated geometry file path
    /// @param outMtlPath Output parameter for the generated MTL file path (OBJ only)
    /// @param forcePLY Force PLY format output even if UVs are present
    /// @param preferPLYIfNoUV Prefer PLY format if no UVs and vertex colors are present
    /// @throws AppException if conversion fails
    DDB_DLL void convertGltfTo3dModel(const std::string &inputGltf,
                                      const std::string &outputBasePath,
                                      std::string &outGeomPath,
                                      std::string &outMtlPath,
                                      bool forcePLY = false,
                                      bool preferPLYIfNoUV = true);

}
#endif // _3D_H
