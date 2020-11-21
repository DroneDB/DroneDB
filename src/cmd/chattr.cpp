/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "chattr.h"

#include <ddb.h>

#include "dbops.h"
#include "exceptions.h"

namespace cmd {
    void Chattr::setOptions(cxxopts::Options& opts) {
        // clang-format off
		opts
			.positional_help("[args]")
            .custom_help("chattr [+-attribute]")
			.add_options()
            ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));
        // clang-format on

        opts.allow_unrecognised_options();
        opts.parse_positional({"working-dir" });
	}

    std::string Chattr::description() {
        return "Manage database attributes";
	}

    std::string Chattr::extendedDescription(){
        return "\r\n\r\nAttributes:\r\n"
               "\tpublic\tmark database as publicly accessible\r\n";
    }

    void Chattr::run(cxxopts::ParseResult& opts) {
        try {
			const auto ddbPath = opts["working-dir"].as<std::string>();
            const auto db = ddb::open(ddbPath, true);

            std::string attribute = "";

            // Manually search for attribute values,
            // the parser cannot handle "-attr" as syntax
            for (int i = 0; i < opts.argc; i++){
                std::string arg(opts.argv[i]);

                if (arg.length() >= 2 && (arg[0] == '-' || arg[0] == '+') && arg[1] != '-'){
                    if (arg != "-w"){
                        attribute = arg;
                    }
                }
            }

            if (attribute.empty()){
                // List attributes
                const auto j = db->getAttributes();
                for (auto &it : j.items()){
                    std::cout << it.key() << ": " << it.value() << std::endl;
                }
            }else{
                if (attribute == "+public" || attribute == "+p"){
                    db->setPublic(true);
                }else if (attribute == "-public" || attribute == "-p"){
                    db->setPublic(false);
                }else{
                    throw ddb::InvalidArgsException("Attribute not valid");
                }
            }
			
		}
        catch (ddb::InvalidArgsException) {
			printHelp();
		}
	}

}


