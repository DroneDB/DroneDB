/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <mio.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {


	void displayEntries(std::vector<Entry>& entries, std::ostream& output, const std::string& format)
	{
		std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs)
		{
			return lhs.path < rhs.path;
		});

		if (format == "text")
		{
			for (auto& e : entries)
			{
				// TODO: Tree?
				// for (auto n = 0; n < e.depth; n++)
				//	output << "\t";
				//output << fs::path(e.path).filename().string() << std::endl;

				output << e.path << std::endl;

			}
		}
		else if (format == "json")
		{
			output << "[";

			bool first = true;

			for (auto& e : entries)
			{
			
				json j;
				e.toJSON(j);
				if (!first) output << ",";
				output << j.dump();

				first = false;
				
			}
			
			output << "]";
		}		
		else
		{
			throw FSException("Unsupported format '" + format + "'");
		}


	}

	void listIndex(Database* db, const std::vector<std::string>& paths, std::ostream& output, const std::string& format, bool recursive, int maxRecursionDepth) {


		if (format != "json" && format != "text")
			throw InvalidArgsException("Invalid format " + format);

		const fs::path directory = rootDirectory(db);

		std::cout << "Root: " << directory << std::endl;
		std::cout << "Max depth: " << maxRecursionDepth << std::endl;
		std::cout << "Recursive: " << recursive << std::endl;

		// If empty we should list everything till max depth
		if (paths.empty())
		{

			

			std::cout << "Listing everything" << std::endl;
			auto entryMatches = getMatchingEntries(db, "*", maxRecursionDepth);

			displayEntries(entryMatches, output, format);

		} else {

			std::cout << "Listing" << std::endl;

			auto pathList = std::vector<fs::path>(paths.begin(), paths.end());

			for (const auto& p : pathList) {

				std::cout << "Path:" << p << std::endl;
				
				auto relPath = io::Path(p).relativeTo(directory);

				std::cout << "Rel path: " << relPath.generic() << std::endl;
				std::cout << "Depth: " << relPath.depth() << std::endl;

				auto entryMatches = getMatchingEntries(db, relPath.generic(), std::max(relPath.depth(), maxRecursionDepth));

				displayEntries(entryMatches, output, format);

			}
		}
	}

}
