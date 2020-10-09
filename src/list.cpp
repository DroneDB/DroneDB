/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <mio.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {


	void displayEntry(Entry& e, std::ostream& output, const std::string& format)
	{
		if (format == "text")
		{
			output << e.path << std::endl;
		}
		else if (format == "json") {
			json j;
			e.toJSON(j);
			output << j.dump();
		}
		else
		{
			throw FSException("Unsupported format '" + format + "'");
		}
	}

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

	void listPath(Database* db, std::ostream& output, const std::string& format, bool recursive, const int maxRecursionDepth, const fs::path
	              & directory, const std::vector<fs::path>::value_type& path)
	{
		LOGD << "Path:" << path;

		auto relPath = io::Path(path).relativeTo(directory);

		LOGD << "Rel path: " << relPath.generic();
		LOGD << "Depth: " << relPath.depth();

		auto entryMatches = getMatchingEntries(db,
		                                       relPath.string().length() == 0 ? "*" : relPath.generic(),
		                                       recursive ? (maxRecursionDepth == -1 ? -1 : std::max(relPath.depth(), maxRecursionDepth)) : relPath.depth());

		// Show the contents of THAT directory
		if (entryMatches.size() == 1)
		{
			const auto firstMatch = entryMatches[0];

			if (firstMatch.type == Directory) {

				LOGD << "The match is folder: " << firstMatch.path;

				// Get all the deeper folders till maxDepth
				entryMatches = getMatchingEntries(db, firstMatch.path, 
					recursive ? (maxRecursionDepth == -1 ? -1 : std::max(firstMatch.depth + 1, maxRecursionDepth)) : firstMatch.depth + 1, 
					true);
				
				displayEntries(entryMatches, output, format);

			} else 
				displayEntries(entryMatches, output, format);
		} else
			displayEntries(entryMatches, output, format);

	}

	void listIndex(Database* db, const std::vector<std::string>& paths, std::ostream& output, const std::string& format, bool recursive, int maxRecursionDepth) {


		if (format != "json" && format != "text")
			throw InvalidArgsException("Invalid format " + format);

		const fs::path directory = rootDirectory(db);

		LOGD << "Root: " << directory;
		LOGD << "Max depth: " << maxRecursionDepth;
		LOGD << "Recursive: " << recursive;

		LOGD << "Listing";

		std::vector<fs::path> pathList;

		if (paths.empty())
			pathList.push_back(directory);
		else
			pathList = std::vector<fs::path>(paths.begin(), paths.end());
		
		for (const auto& path : pathList) {

			listPath(db, output, format, recursive, maxRecursionDepth, directory, path);

		}
		
	}

}
