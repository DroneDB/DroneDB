/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GEOPROJECT_H
#define GEOPROJECT_H

#include "../classes/database.h"

namespace ddb {

void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize = "");

}

#endif // GEOPROJECT_H
