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
#include "command.h"
#include "../logger.h"

namespace cmd {

Command::Command() {
}

void Command::run(int argc, char *argv[]) {
    cxxopts::Options opts(argv[0], description());
    opts
    .show_positional_help();

    setOptions(opts);
    opts.add_options()
    ("h,help", "Print help")
    ("v,verbose", "Show verbose output");
    help = opts.help({""});

    try{
        auto result = opts.parse(argc, argv);

        if (result.count("help")) {
            printHelp();
        }

        if (result.count("verbose")) {
            set_logger_verbose();
        }

        run(result);
    }catch(const cxxopts::option_not_exists_exception &){
        printHelp();
    }
}

void Command::printHelp() {
    std::cout << help;
    exit(0);
}

}
