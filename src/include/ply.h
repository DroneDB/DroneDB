/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PLY_H
#define PLY_H

#include <vector>
#include <string>
#include "fs.h"
#include "entry_types.h"
#include "ddb_export.h"

namespace ddb
{

    struct PlyInfo
    {
        unsigned long vertexCount;
        bool isMesh;
        bool hasTextures;
        // True when the vertex element carries 3D Gaussian Splat attributes
        // (spherical-harmonic DC term and/or anisotropic scale + rotation + opacity)
        // and there is no face element. Mutually exclusive with isMesh.
        bool isSplat;
        // Spherical-harmonics degree (0..3) inferred from the number of f_rest_*
        // properties. -1 when the file is not a Gaussian Splat.
        int shDegree;
        std::vector<std::string> dimensions;
    };

    DDB_DLL EntryType identifyPly(const fs::path &plyFile);
    DDB_DLL bool getPlyInfo(const fs::path &plyFile, PlyInfo &info);

}
#endif // PLY_H
