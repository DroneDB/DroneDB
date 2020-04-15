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

#include "cmd/list.h"
#include "logger.h"
#include "classes/exceptions.h"
#include "classes/database.h"
#include "classes/exif.h"
#include "libs/ddb.h"
#include "gdal_priv.h"


using namespace std;
using namespace ddb;

[[ noreturn ]] void printHelp(char *argv[]) {
    std::cout << "DroneDB v" << ddb::getVersion() << " - Easily manage and share aerial datasets :)" << std::endl <<
              "Usage:" << std::endl <<
              "	" << argv[0] << " <command> [args] [PATHS]" << std::endl << std::endl <<
              "Commands:" << std::endl;
    for (auto &cmd : cmd::commands){
        std::cout << "	" << cmd.first << " - " << cmd.second->description() << std::endl;
    }
    std::cout << std::endl <<
              "	-h, --help		Print help" << std::endl <<
              "	--version		Print version" << std::endl << std::endl <<
              "For detailed command help use: " << argv[0] << " <command> --help " << std::endl <<
              "See https://uav4geo.com for more information." << std::endl;
    exit(0);
}

bool hasParam(int argc, char *argv[], const char* param) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], param) == 0) return true;
    }
    return false;
}

int main(int argc, char* argv[]) {
    ddb::initialize();
    if (hasParam(argc, argv, "--debug")) {
        set_logger_verbose();
    }

    LOGV << "DDB v" << ddb::getVersion();
    LOGV << "SQLite version: " << sqlite3_libversion();
    LOGV << "SpatiaLite version: " << spatialite_version();
    LOGV << "GDAL version: " << GDALVersionInfo("RELEASE_NAME");

    if (argc <= 1) printHelp(argv);
    else {
        std::string cmdKey = std::string(argv[1]);

        if (cmdKey == "--help" || cmdKey == "-h") {
            printHelp(argv);
        }

        if (hasParam(argc, argv, "--version")) {
            std::cout << ddb::getVersion() << std::endl;
            exit(0);
        } else {
            auto aliasIter = cmd::aliases.find(cmdKey);
            if (aliasIter != cmd::aliases.end()) {
                cmdKey = aliasIter->second;
            }

            auto cmdIter = cmd::commands.find(cmdKey);
            if (cmdIter == cmd::commands.end()) {
                printHelp(argv);
            }

            auto command = cmdIter->second;
            try {
                // Run command
                argv[1] = argv[0];
                command->run(argc - 1, argv + 1);
            } catch (const AppException &exception) {
                std::cerr << exception.what() << std::endl;
                return 1;
            }
        }
    }

    return 0;
}


