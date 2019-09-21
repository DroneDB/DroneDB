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

#include "cmd/command.h"
#include "logger.h"
#include "database.h"
#include "exceptions.h"

#include "cmd/build.hpp"

#define VERSION "0.9.0"

using namespace std;

cxxopts::Options getDefaultOptions(char *argv[]) {
    cxxopts::Options options(argv[0], "DDB v" VERSION " - Aerial data management utility");
    options
    .positional_help("[args] [PATH]")
    .custom_help("<init|add|rm|status|commit|build>")
    .show_positional_help()
    .add_options()
    ("command", "Command", cxxopts::value<std::string>())
    ("h,help", "Print help")
    ("v,verbose", "Show verbose output")
    ("version", "Show version");

    options.parse_positional({"command"});

    return options;
}


//        options
//        .add_options()
//        ("i,input", "Input directory", cxxopts::value<std::vector<std::string>>())
//        ("o,output", "Output file", cxxopts::value<std::string>())



//options.parse_positional({"command", "input", "output"});

int main(int argc, char* argv[]) {
    init_logger();

    auto opts = getDefaultOptions(argv);
    try {
        auto result = opts.parse(argc, argv);
        std::string command = result.count("command") ? result["command"].as<std::string>() : "";

        if (result.count("help") || command == "help" || command == "") {
            std::cout << opts.help({""});
            exit(0);
        }

        std::cout << command;

    } catch (const cxxopts::OptionException& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        exit(1);
    }

    return 0;

    // ------ OLD -----------
    /*
        if (result.count("verbose")) set_logger_verbose();

        if (result.count("version")) {
            std::cout << VERSION;
            exit(0);
        }

        try {
            // Initialization steps
            Database::Initialize();

            LOGV << "DDB v" VERSION;
            LOGV << "SQLite version: " << sqlite3_libversion();
            LOGV << "SpatiaLite version: " << spatialite_version();

            auto cmd = result["command"].as<std::string>();
            if (cmd == "build") {
                if (result.count("input")) {
    //                cmd::Build(result["input"].as<std::string>());
                } else {
                    throw InvalidArgsException("No input path specified");
                }
            } else {
                throw InvalidArgsException("Invalid command \"" + cmd + "\"");
            }

            if (result.count("command")) {
                std::cout << "Command = " << result["command"].as<std::string>()
                          << std::endl;
            }

            if (result.count("input")) {
                std::cout << "Input = " << result["input"].as<std::string>()
                          << std::endl;
            }

            if (result.count("output")) {
                std::cout << "Output = " << result["output"].as<std::string>()
                          << std::endl;
            }
        } catch (const InvalidArgsException &exception) {
            LOGE << exception.what() << ". Run ./dbb --help for usage information.";
        } catch (const AppException &exception) {
            LOGF << exception.what();
            return 1;
        }
    */
}
