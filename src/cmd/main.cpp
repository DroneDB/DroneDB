/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>

#include "cmd/cmdlist.h"
#include "logger.h"
#include "exceptions.h"
#include "database.h"
#include "exif.h"
#include "ddb.h"
#include "dbops.h"
#include "mio.h"
#include <gdal_priv.h>
#include <curl/curl.h>

using namespace std;
using namespace ddb;

[[ noreturn ]] void printHelp(char *argv[]) {
    std::cout << "DroneDB v" << DDBGetVersion() << " - Effortless aerial data management and sharing" << std::endl <<
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
              "See https://docs.dronedb.app for more information." << std::endl;
    exit(0);
}

bool hasParam(int argc, char *argv[], const char* param) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], param) == 0) return true;
    }
    return false;
}


int main(int argc, char* argv[]) {
    memcpy(argv[0], "ddb\0", 4);
    DDBRegisterProcess(hasParam(argc, argv, "--debug"));

    LOGV << "DDB v" << DDBGetVersion();
    LOGV << "SQLite version: " << sqlite3_libversion();
    LOGV << "SpatiaLite version: " << spatialite_version();
    LOGV << "GDAL version: " << GDALVersionInfo("RELEASE_NAME");
    LOGV << "CURL version: " << curl_version();

    if (argc <= 1) printHelp(argv);
    else {
        std::string cmdKey = std::string(argv[1]);

        if (cmdKey == "--help" || cmdKey == "-h") {
            printHelp(argv);
        }

        if (hasParam(argc, argv, "--version")) {
            std::cout << DDBGetVersion() << std::endl;
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


