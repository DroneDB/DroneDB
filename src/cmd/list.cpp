/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "list.h"

#include <ddb.h>


#include "fs.h"
#include "dbops.h"

#include "exceptions.h"
#include "basicgeometry.h"

namespace cmd {

	void List::setOptions(cxxopts::Options& opts) {
		opts
			.positional_help("[args]")
			.custom_help("list *.JPG")
			.add_options()
			("i,input", "File(s) to list", cxxopts::value<std::vector<std::string>>())
			("o,output", "Output file to write results to", cxxopts::value<std::string>()->default_value("stdout"))
			("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
			("m,maxdepth", "Max recursion depth", cxxopts::value<int>()->default_value("-1"))
			("f,format", "Output format (text|json)", cxxopts::value<std::string>()->default_value("text"));

		opts.parse_positional({ "input" });
	}

	std::string List::description() {
		return "List indexed files and directories";
	}

	void List::run(cxxopts::ParseResult& opts) {

		try {

			const auto ddbPath = opts["working-dir"].as<std::string>();
			const auto paths = opts.count("input") > 0 ? opts["input"].as<std::vector<std::string>>() : std::vector<std::string>();
			const auto format = opts["format"].as<std::string>();
			const auto maxRecursionDepth = opts["maxdepth"].as<int>();

			const auto db = ddb::open(std::string(ddbPath), true);

			if (opts.count("output")) {
				const std::string filename = opts["output"].as<std::string>();
				std::ofstream file(filename, std::ios::out | std::ios::trunc | std::ios::binary);
				if (!file.is_open()) throw ddb::FSException("Cannot open " + filename);

				listIndex(db.get(), paths, file, format, maxRecursionDepth);

				file.close();
			}
			else {
				listIndex(db.get(), paths, std::cout, format, maxRecursionDepth);
			}

		}
		catch (ddb::InvalidArgsException) {
			printHelp();
		}
	}

}


