/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COG_H
#define COG_H

#include <string>

#include "basicgeometry.h"
#include "ddb_export.h"
#include "json.h"

namespace ddb {

DDB_DLL void buildCog(const std::string& inputGTiff, const std::string& outputCog);

}

#endif  // COG_H
