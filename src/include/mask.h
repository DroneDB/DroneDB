/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MASK_H
#define MASK_H

#include <string>

#include "ddb_export.h"

namespace ddb {

DDB_DLL void maskBorders(const std::string &input,
                         const std::string &output,
                         int nearDist = 15,
                         bool white = false,
                         const std::string &color = "");

}

#endif  // MASK_H
