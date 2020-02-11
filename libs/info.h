/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
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
