/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VECTOR_H
#define VECTOR_H

#include <string>
#include "ddb_export.h"
// #include "json.h"
// #include "basicgeometry.h"

namespace ddb
{

    DDB_DLL void buildVector(const std::string &input, const std::string &outputVector, bool overwrite = false);

    DDB_DLL bool convertToFlatGeobuf(const std::string &input, const std::string &output);

    DDB_DLL std::vector<std::string> getVectorDependencies(const std::string &input);

}

#endif // VECTOR_H