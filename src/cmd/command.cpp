/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "command.h"
#include "logger.h"
#include "exceptions.h"

namespace cmd {

Command::Command() {
}

cxxopts::Options Command::genOptions(const char *programName){
    cxxopts::Options opts(programName, description() + extendedDescription());
    opts
    .show_positional_help();

    setOptions(opts);
    opts.add_options()
    ("h,help", "Print help")
    ("debug", "Show debug output");

    return opts;
}

void Command::run(int argc, char *argv[]) {
    cxxopts::Options opts = genOptions(argv[0]);

    try{
        auto result = opts.parse(argc, argv);

        if (result.count("help")) {
            printHelp();
        }

        if (result.count("debug")) {
            set_logger_verbose();
        }

        run(result);
    }catch(const cxxopts::option_not_exists_exception &){
        printHelp();
    }catch(const cxxopts::argument_incorrect_type &){
        printHelp();
    }catch(const cxxopts::option_requires_argument_exception &){
        printHelp();
    }catch(const ddb::AppException &e){
        std::cerr << e.what() << std::endl;
        exit(EXIT_FAILURE);
    }
}

void Command::printHelp(std::ostream &out, bool exitAfterPrint) {
    out << genOptions().help({""});
    if (exitAfterPrint) exit(0);
}

}
