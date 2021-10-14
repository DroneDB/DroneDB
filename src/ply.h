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

namespace ddb{

struct PlyInfo{
    unsigned long vertexCount;
    bool isMesh;
    std::vector<std::string> dimensions;
};

DDB_DLL EntryType identifyPly(const fs::path &plyFile);
DDB_DLL bool getPlyInfo(const fs::path &plyFile, PlyInfo &info);

}
#endif // PLY_H
