/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CLI_H
#define CLI_H

#include <iostream>
#include "command.h"

namespace cmd
{

    // Runs a command on its arguments (argv[0] is the program) and turns whatever it throws
    // into a message on `err` and an exit code: the one place that does it, so no command
    // exits the process or prints its own error.
    //
    //   UsageError, a cxxopts parse error, ddb::InvalidArgsException
    //                           "error: <message>", a blank line and the usage of the command
    //   any other ddb::AppException   the message
    //   any other std::exception      "error: <message>"
    //
    // all with EXIT_FAILURE; a command that returns normally gives its own exit code. Nothing
    // is written to stdout: a script reading `ddb info -f json` gets the output of the command
    // and nothing else.
    int execute(Command &command, int argc, char *argv[], std::ostream &err = std::cerr);

}

#endif // CLI_H
