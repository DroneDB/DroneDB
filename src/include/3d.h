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

}
#endif // _3D_H
