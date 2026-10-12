/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_PROCESS_TEST_UTILS_H
#define DDB_PROCESS_TEST_UTILS_H

#include "mio.h"

inline fs::path testChildPath() {
    return ddb::io::getExeFolderPath() / DDB_TEST_CHILD_NAME;
}

#endif
