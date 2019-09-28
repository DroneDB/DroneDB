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
#ifndef INDEX_H
#define INDEX_H

#include <filesystem>
#include "../classes/database.h"
#include "types.h"
#include "../classes/exif.h"
#include "../classes/hash.h"
#include "../classes/database.h"
#include "../classes/exceptions.h"
#include "../utils.h"
#include "entry.h"

namespace fs = std::filesystem;

namespace ddb{

std::string create(const std::string &directory);
std::unique_ptr<Database> open(const std::string &directory, bool traverseUp);
void addToIndex(Database *db, const std::vector<std::string> &paths);
void updateIndex(const std::string &directory);


}


#endif // INDEX_H
