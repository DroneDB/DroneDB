/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <authcredentials.h>
#include <mio.h>
#include <registryutils.h>
#include <userprofile.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

void pull(const std::string& registry) {


    /*
    
    -- Pull Workflow --

    1) Get our tag using tagmanager
    2) Get our last sync time for that specific registry using syncmanager
    3) Get dataset mtime
        3.1) Authenticate if not
        3.2) Call endpoint
    4) Alert if dataset_mtime < last_sync (it means we have more recent changes than server, so the pull is pointless or potentially dangerous)
    5) Get ddb from registry
        5.1) Call endpoint
        5.2) Unzip archive in temp folder
    6) Perform local diff using delta method
    7) Download all the missing files
        7.1) Call download endpoint with file list (to test)
        7.2) Unzip archive in temp folder
    8) Apply changes to local files
    9) Replace ddb database
    10) Update last sync time
    
    */

    LOGD << "Pull from " << registry;

}

}  // namespace ddb
