/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "build.h"
#include "exceptions.h"

namespace ddb {

void build_all(const std::string& output) {
    LOGD << "In build_all('" << output << "')";
}

void build(const std::string& path, const std::string& output){
    LOGD << "In build('" << path << "','" << output << "')";
        
}

}
