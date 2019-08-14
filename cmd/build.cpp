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

#include <iostream>
#include <experimental/filesystem>
#include "build.hpp"
#include "../database.h"
#include "../exceptions.h"
#include "../logger.h"

namespace fs = std::experimental::filesystem;

void cmd::Build(const std::string &directory) {
    fs::path dirPath = directory;
    fs::path ddbDirPath = dirPath / ".ddb";
    fs::path dbasePath = ddbDirPath / "dbase";

    bool ddbPathExists = false;
    bool dbaseExists = false;

    LOGD << "CMD::BUILD: Checking if .ddb directory exists...";
    if (fs::exists(ddbDirPath)) {
        LOGD << "CMD::BUILD: " << ddbDirPath.u8string() + " exists";
        ddbPathExists = true;
    } else {
        if (fs::create_directory(ddbDirPath)) {
            LOGD << "CMD::BUILD: " << ddbDirPath.u8string() + " created";
        } else {
            throw FSException("Cannot create directory: " + ddbDirPath.u8string() + ". Check that you have permissions.");
        }
    }

    LOGD << "CMD::BUILD: Checking if dbase exists...";
    if (fs::exists(dbasePath)) {
        LOGD << "CMD::BUILD: " << dbasePath.u8string() + " exists";
        dbaseExists = true;
    } else {
        LOGD << "CMD::BUILD: " << dbasePath.u8string() + " is new";
    }

    try {
        // Open/create database
        std::unique_ptr<Database> db = std::make_unique<Database>();
        db->open(dbasePath.u8string());

        if (!db->tableExists("meta")) {
            db->createTables();
        } else {
            LOGD << "CMD::BUILD: " << "meta table exists";
        }

        // fs::directory_options::skip_permission_denied
        for(auto& p: fs::recursive_directory_iterator(directory)) {
            //std::cout << p.path() << '\n';
        }
    } catch (const AppException &exception) {
        LOGV << "Exception caught, cleaning up...";

        // Remove stale database
        if (!dbaseExists) {
            if (!fs::remove(dbasePath)) {
                LOGE << "Cannot cleanup " << dbasePath.u8string();
            }
        }

        // Remove directory we just created
        if (!ddbPathExists) {
            if (!fs::remove(ddbDirPath)) {
                LOGE << "Cannot cleanup " << ddbDirPath.u8string();
            }
        }

        throw exception;
    }
}
