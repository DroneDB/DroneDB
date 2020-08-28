/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef INFO_H
#define INFO_H

#include "entry.h"
#include "ddb_export.h"

namespace ddb {

DDB_DLL void info(const std::vector<std::string> &input, std::ostream &output,
                  const std::string &format = "text", bool recursive = false, int maxRecursionDepth = 0, const std::string &geometry = "auto",
                  bool withHash = false, bool stopOnError = true);

}

#endif // INFO_H
