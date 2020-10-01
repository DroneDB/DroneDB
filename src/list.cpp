/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "dbops.h"
#include "list.h"
#include "exceptions.h"

namespace ddb {

void list(const std::string &input) {
    std::cout << "Listing: " << input << std::endl;
}

}
