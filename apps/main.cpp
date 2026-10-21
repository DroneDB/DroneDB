/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cstdlib>
#include <iostream>
#include "gdal_inc.h"

#include "cmd/include/cmdlist.h"
#include "cmd/include/gendocs.h"
#include "logger.h"
#include "exceptions.h"
#include "database.h"
#include "exif.h"
#include "ddb.h"
#include "dbops.h"
#include "mio.h"
#include "crash_handlers.h"

using namespace std;
using namespace ddb;

// Explicit help (-h/--help) goes to stdout and exits 0; a missing or unknown command is a
// usage error: stderr and EXIT_FAILURE.
[[noreturn]] void printHelp(char *argv[], std::ostream &out = std::cout, int exitCode = EXIT_SUCCESS)
{
    out << "DroneDB v" << DDBGetVersion() << " - Effortless aerial data management and sharing" << std::endl
              << "Usage:" << std::endl
              << "	" << argv[0] << " <command> [args] [PATHS]" << std::endl
              << std::endl
              << "Commands:" << std::endl;
    for (auto &cmd : cmd::commands)
    {
        out << "	" << cmd.first << " - " << cmd.second->description() << std::endl;
    }
    out << std::endl
              << "	-h, --help		Print help" << std::endl
              << "	--version		Print version" << std::endl
              << std::endl
              << "For detailed command help use: " << argv[0] << " <command> --help " << std::endl
              << "See https://docs.dronedb.app for more information." << std::endl;
    exit(exitCode);
}

bool hasParam(int argc, char *argv[], const char *param)
{
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], param) == 0)
            return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    memcpy(argv[0], "ddb\0", 4);

    // One command per process: open and close indexes directly (no idle connections)
#ifdef _WIN32
    if (std::getenv("DDB_DB_POOL_SIZE") == nullptr)
        _putenv_s("DDB_DB_POOL_SIZE", "0");
#else
    setenv("DDB_DB_POOL_SIZE", "0", 0);
#endif
    if (DDBRegisterProcess(hasParam(argc, argv, "--debug")) != DDBERR_NONE)
    {
        std::cerr << DDBGetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    installCrashHandlers();

    // Super secret command to generate docs
    if (hasParam(argc, argv, "--gendocs"))
    {
        cmd::generateDocs(argc, argv);
        return EXIT_SUCCESS;
    }

    if (argc <= 1)
        printHelp(argv, std::cerr, EXIT_FAILURE);
    else
    {
        std::string cmdKey = std::string(argv[1]);

        if (cmdKey == "--help" || cmdKey == "-h")
        {
            printHelp(argv);
        }

        if (hasParam(argc, argv, "--version"))
        {
            std::cout << DDBGetVersion() << std::endl;
            exit(0);
        }
        else
        {
            auto aliasIter = cmd::aliases.find(cmdKey);
            if (aliasIter != cmd::aliases.end())
            {
                cmdKey = aliasIter->second;
            }

            auto cmdIter = cmd::commands.find(cmdKey);
            if (cmdIter == cmd::commands.end())
            {
                std::cerr << "Unknown command: " << cmdKey << std::endl
                          << std::endl;
                printHelp(argv, std::cerr, EXIT_FAILURE);
            }

            auto command = cmdIter->second;
            try
            {
                // Run command
                argv[1] = argv[0];
                command->run(argc - 1, argv + 1);
            }
            catch (const AppException &exception)
            {
                std::cerr << exception.what() << std::endl;
                return 1;
            }
        }
    }

    return 0;
}
