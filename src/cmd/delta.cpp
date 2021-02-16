/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "delta.h"

#include <ddb.h>

#include "dbops.h"
#include "exceptions.h"

namespace cmd {
    void Delta::setOptions(cxxopts::Options& opts) {
        // clang-format off
        opts
			.positional_help("[args]")
			.custom_help("delta source target")
			.add_options()
			("s,source", "Source ddb", cxxopts::value<std::string>())
			("t,target", "Target ddb", cxxopts::value<std::string>()->default_value("."))
			("f,format", "Output format (text|json)", cxxopts::value<std::string>()->default_value("text"));
        // clang-format on
		opts.parse_positional({ "source", "target" });

	}

    std::string Delta::description() {
        return "Generate delta between two ddb databases";
	}

    std::string Delta::extendedDescription(){
        return "\r\n\r\nOutputs the delta that applied to target turns it into source";
    }

    void Delta::run(cxxopts::ParseResult& opts) {
        try {

            if(opts["source"].count() != 1) {
                printHelp();
                return;
            }

            const auto sourceDdbPath = opts["source"].as<std::string>();
            const auto targetDdbPath = opts["target"].as<std::string>();
            const auto format = opts["format"].as<std::string>();

            LOGD << "Source: " << sourceDdbPath;
            LOGD << "Target: " << targetDdbPath;
            LOGD << "Format: " << format;

            const auto source = ddb::open(sourceDdbPath, false);
            const auto target = ddb::open(targetDdbPath, false);

            delta(source.get(), target.get(), std::cout, format);            
			
		}
        catch (ddb::InvalidArgsException) {
			printHelp();
		}
	}

}


