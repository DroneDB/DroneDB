/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "gdal_inc.h"

#include "cmd/include/cli.h"
#include "cmd/include/cmdlist.h"
#include "cmd/include/gendocs.h"
#include "logger.h"
#include "exceptions.h"
#include "database.h"
#include "exif.h"
#include "ddb.h"
#include "dbops.h"
#include "mio.h"
#include "process_init.h"
#include "crash_handlers.h"

using namespace std;
using namespace ddb;

// The list of commands: explicit help (-h/--help) goes to stdout and returns EXIT_SUCCESS; a missing
// or unknown command is a usage error: stderr and EXIT_FAILURE.
static int printHelp(std::ostream &out, int exitCode)
{
    const auto &registry = cmd::CommandRegistry::builtin();
    out << "DroneDB v" << DDBGetVersion() << " - Effortless aerial data management and sharing" << std::endl
        << "Usage:" << std::endl
        << "	" << cmd::ProgramName << " <command> [args] [PATHS]" << std::endl
        << std::endl
        << "Commands:" << std::endl;
    for (const auto &name : registry.names())
    {
        out << "	" << name << " - " << registry.create(name)->description() << std::endl;
    }
    out << std::endl
        << "	-h, --help		Print help" << std::endl
        << "	--version		Print version" << std::endl
        << std::endl
        << "For detailed command help use: " << cmd::ProgramName << " <command> --help " << std::endl
        << "See https://docs.dronedb.app for more information." << std::endl;
    return exitCode;
}

static bool hasParam(int argc, char *argv[], const char *param)
{
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], param) == 0)
            return true;
    }
    return false;
}

// argv in UTF-8 (mio.h, "Path encoding")
static int runCommand(int argc, char *argv[])
{
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
        return printHelp(std::cerr, EXIT_FAILURE);

    const std::string cmdKey(argv[1]);
    if (cmdKey == "--help" || cmdKey == "-h")
        return printHelp(std::cout, EXIT_SUCCESS);

    if (hasParam(argc, argv, "--version"))
    {
        std::cout << DDBGetVersion() << std::endl;
        return EXIT_SUCCESS;
    }

    const auto command = cmd::CommandRegistry::builtin().create(cmdKey);
    if (!command)
    {
        std::cerr << "Unknown command: " << cmdKey << std::endl
                  << std::endl;
        return printHelp(std::cerr, EXIT_FAILURE);
    }

    // The command sees the program name and its own arguments
    std::vector<char *> args{argv[0]};
    args.insert(args.end(), argv + 2, argv + argc);
    return cmd::execute(*command, static_cast<int>(args.size()), args.data());
}

// The command, then the process teardown while every thread still runs (process_init.h)
static int ddbMain(int argc, char *argv[])
{
    const int code = runCommand(argc, argv);
    ddb::finalizeProcess();
    return code;
}

#ifdef _WIN32
// The CRT passes main its arguments in the ANSI code page: wmain receives them in UTF-16 (split
// by the same CRT rules) and hands them to ddbMain in UTF-8
int wmain(int argc, wchar_t *wargv[])
{
    std::vector<std::string> args;
    try
    {
        for (int i = 0; i < argc; i++)
            args.push_back(ddb::io::fromWide(wargv[i]));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Invalid command line: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    return ddbMain(argc, argv.data());
}
#else
int main(int argc, char *argv[])
{
    return ddbMain(argc, argv);
}
#endif
