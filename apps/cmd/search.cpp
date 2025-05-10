/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "include/search.h"

#include <ddb.h>

#include "fs.h"
#include "dbops.h"

#include "exceptions.h"

namespace cmd
{

	void Search::setOptions(cxxopts::Options &opts)
	{
		// clang-format off
		opts
			.positional_help("[args]")
            .custom_help("search '*file*'")
            .add_options()
            ("q,query", "Search query", cxxopts::value<std::string>())
			("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
			("f,format", "Output format (text|json)", cxxopts::value<std::string>()->default_value("text"));
		// clang-format on
		opts.parse_positional({"query"});
	}

	std::string Search::description()
	{
		return "Search indexed files and directories";
	}

	void Search::run(cxxopts::ParseResult &opts)
	{

		try
		{

			const auto ddbPath = opts["working-dir"].as<std::string>();
			const auto query = opts.count("query") > 0 ? opts["query"].as<std::string>() : "%";
			const auto format = opts["format"].as<std::string>();

			const auto db = ddb::open(std::string(ddbPath), true);

			searchIndex(db.get(), query, std::cout, format);
		}
		catch (ddb::InvalidArgsException)
		{
			printHelp();
		}
	}

}
