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

#include "ddb.h"
#include "entry.h"
#include "info.h"
#include "../classes/exceptions.h"

namespace ddb {

using json = nlohmann::json;

void getFilesInfo(const std::vector<std::string> &input, const std::string &format, std::ostream &output){
    auto filePaths = getPathList("/", input, false);

    if (format == "json"){
        output << "[";
    }else if (format == "text"){
        // Nothing
    }else{
        throw InvalidArgsException("Invalid --format " + format);
    }

    json j;
    for (auto &fp : filePaths){
        LOGD << "Parsing entry " << fp.u8string();

        Entry e;
        parseEntry(fp, ".", e);

        if (format == "json"){
            e.toJSON(j);
            output << j.dump();
        }else{
            output << e.toString() << "\n";
        }
    }

    if (format == "json"){
        output << "]";
    }
}

}
