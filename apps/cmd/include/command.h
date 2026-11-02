/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COMMAND_H
#define COMMAND_H

#include <cstdlib>
#include <cxxopts.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include "json_fwd.h"
#include "options.h"

namespace cmd
{

    // The name the usage texts show: the program is reached as ddb, ddbcmd or a full path
    constexpr const char *ProgramName = "ddb";

    // A command line that cannot be run as typed (a missing or invalid argument). Thrown by
    // Command::usageError(); the handler of the CLI (cli.h) prints the message, when there is
    // one, and the usage of the command on stderr, and ends with EXIT_FAILURE.
    class UsageError : public std::runtime_error
    {
    public:
        explicit UsageError(const std::string &message = "") : std::runtime_error(message) {}
    };

    // A CLI command. Commands never exit the process: run() returns the exit code and
    // reports a failure by throwing (UsageError, ddb::AppException), which the handler of the
    // CLI maps to stderr and an exit code. The output of the command (and nothing else) goes
    // to stdout, so a script can read `ddb info -f json`.
    class Command
    {
    private:
        cxxopts::Options genOptions(const char *programName = ProgramName);

    protected:
        // The work of the command, after its options were parsed: the exit code
        virtual int run(cxxopts::ParseResult &opts) = 0;
        virtual void setOptions(cxxopts::Options &opts) = 0;

        void output(std::ostream &out, const json &j, ddb::OutputFormat format);
        void printJsonToText(std::ostream &out, const json &j);

        // Missing or invalid arguments: throws UsageError(message)
        [[noreturn]] void usageError(const std::string &message = "");

    public:
        Command() = default;
        virtual ~Command() = default;

        // Parses argv (argv[0] is the program, skipped) and runs the command: the exit code.
        // --help prints the usage on stdout and returns EXIT_SUCCESS. Throws what the command
        // throws, and cxxopts' exceptions for options that do not parse.
        int run(int argc, char *argv[]);

        virtual std::string description() { return ""; }
        virtual std::string extendedDescription() { return ""; }

        // The usage of the command
        void printHelp(std::ostream &out = std::cout);
    };

}

#endif // COMMAND_H
