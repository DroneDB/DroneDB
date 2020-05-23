/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef INFO_H
#define INFO_H

#include "entry.h"

namespace ddb {

struct ParseFilesOpts{
    std::string format;
    bool recursive;
    int maxRecursionDepth;

    entry::ParseEntryOpts peOpts;

    ParseFilesOpts() :
        format("text"), recursive(false), maxRecursionDepth(0) {};
};

void parseFiles(const std::vector<std::string> &input, std::ostream &output, ParseFilesOpts &opts);

}

#endif // INFO_H
