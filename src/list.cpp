/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <mio.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

	// Supported formats: json and text
	void listIndex(Database* db, const std::vector<std::string>& paths, std::ostream& output, const std::string& format, int maxRecursionDepth) {

		if (paths.empty())
		{
			// Nothing to do
			LOGD << "No paths provided";
			return;
		}

		if (format != "json" && format != "text")
			throw InvalidArgsException("Invalid format " + format);

		const fs::path directory = rootDirectory(db);

		std::cout << "Root: " << directory << std::endl;

		auto pathList = std::vector<fs::path>(paths.begin(), paths.end());

		for (const auto& p : pathList) {

			// std::cout << "Parsing entry " << p << std::endl;

			auto relPath = io::Path(p).relativeTo(directory);

			// std::cout << "Rel path: " << relPath.generic() << std::endl;

			auto entryMatches = getMatchingEntries(db, relPath.generic(), maxRecursionDepth);


			for (auto& e : entryMatches)
			{

				std::cout << "Match: " << e.path << std::endl;
				
				// If it's a folder we should get all the subfolders till the maxDepth

			}

		}

	}

}
