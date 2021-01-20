/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GEOPROJECT_H
#define GEOPROJECT_H

#include "database.h"
#include "ddb_export.h"

namespace ddb {

typedef std::function<bool(const std::string &imageWritten)>
    GeoProjectCallback;

DDB_DLL void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize = "", bool stopOnError = false, const GeoProjectCallback &callback = nullptr);

}

#endif // GEOPROJECT_H
