/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <fstream>
#include "include/password.h"
#include <passwordmanager.h>

#include <ddb.h>

#include "fs.h"
#include "dbops.h"

#include "exceptions.h"
#include "basicgeometry.h"

namespace cmd
{

	void Password::setOptions(cxxopts::Options &opts)
	{
		// clang-format off
		opts
			.positional_help("[args]")
			.custom_help("password [a,append|v,verify|c,clear] [password]");
		cmd::addWorkingDirOption(opts);
		opts.add_options()
			("c,command", "Command to execute", cxxopts::value<std::string>())
			("a,argument", "Command argument", cxxopts::value<std::string>()->default_value(""));
		// clang-format on

		opts.parse_positional({"command", "argument"});
	}

	std::string Password::description()
	{
		return "Manage database passwords";
	}

	int Password::run(cxxopts::ParseResult &opts)
	{

		const auto ddbPath = opts["working-dir"].as<std::string>();

		LOGD << "DDB Path: " << ddbPath;

		if (opts.count("command") != 1)
		{
			usageError("Missing command");
		}

		const auto command = opts["command"].as<std::string>();
		const auto argument = opts["argument"].as<std::string>();

		LOGD << "Command: '" << command << "'";
		LOGD << "Argument: '" << argument << "'";

		const auto db = ddb::open(ddbPath, true);
		ddb::PasswordManager manager(db.get());

		if (command == "a" || command == "append")
		{
			if (argument.empty())
			{
				usageError("Missing parameter in append command");
			}

			manager.append(argument);

			std::cout << std::endl
					  << "Password added to database" << std::endl;
		}
		else if (command == "v" || command == "verify")
		{
			const auto res = manager.verify(argument);

			std::cout << std::endl
					  << (res ? "Password verification succeeded" : "Password verification failed") << std::endl;
			if (!res)
				return EXIT_FAILURE;
		}
		else if (command == "c" || command == "clear")
		{
			manager.clearAll();

			std::cout << std::endl
					  << "Deleted all passwords" << std::endl;
		}
		else
		{
			usageError("Unknown command: " + command);
		}

		return EXIT_SUCCESS;
	}

}
