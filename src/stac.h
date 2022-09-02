/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STAC_H
#define STAC_H

#include <string>
#include "ddb_export.h"
#include "dbops.h"
#include <vector>

namespace ddb{

DDB_DLL std::string generateStac(const std::vector<std::string> &paths,
                                 const std::string &entry = "",
                                 const std::string &matchExpr = "",
                                 bool recursive = false,
                                 int maxRecursionDepth = 2,
                                 const std::string &endpoint = "./stac",
                                 const std::string &id = "");

}

#endif // STAC_H
