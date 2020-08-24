/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TESTAREA_H
#define TESTAREA_H

#include <string>
#include "net.h"
#include "fs.h"

using namespace ddb;

class TestArea
{
    std::string name;
public:
    TestArea(const std::string &name, bool recreateIfExists = false);

    fs::path getFolder(const fs::path &subfolder = "");
    fs::path downloadTestAsset(const std::string &url, const std::string &filename, bool overwrite = false);
};

#endif // TESTAREA_H
