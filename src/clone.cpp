/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */


#include <mio.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {


void  clone(const std::string& target, const std::string& folder) {

    std::cout << "Target = " << target << std::endl;
    std::cout << "Folder = " << folder << std::endl;

    // Workflow
    // 1) Generate download url
    // 1.1) If full url was not provided, generate it starting from tag (default remote is DEFAULT_REGISTRY)
    // 2) Download zip in temp folder
    // 3) Create target folder
    // 3.1) If target folder already exists throw error
    // 4) Unzip in target folder
    // 5) Remove temp zip
    // 6) Update sync information
}

}

