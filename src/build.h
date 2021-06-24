/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_H
#define BUILD_H

//#include "entry.h"
#include "ddb_export.h"

namespace ddb {

#define DEFAULT_BUILD_PATH "out_dir"

DDB_DLL void build_all(const std::string& output);
DDB_DLL void build(const std::string& path, const std::string& output);

}

#endif // BUILD_H