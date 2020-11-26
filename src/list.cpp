/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <mio.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {


	void displayEntry(const Entry& e, std::ostream& output, const std::string& format)
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

	void getBaseEntries(Database* db, std::vector<fs::path> pathList, fs::path rootDirectory, bool& expandFolders, std::vector<Entry>& baseEntries)
	{

		for (const fs::path& path : pathList) {

			LOGD << "Path: " << path;

            io::Path relPath = io::Path(path).relativeTo(rootDirectory);

            auto pathStr = relPath.generic();

			LOGD << "Rel path: " << pathStr;

			// Let's expand only if we were asked to list a different folder
			expandFolders = expandFolders || pathStr.length() > 0; //ourPath.generic() != pathStr;

			const auto depth = static_cast<int>(count(pathStr.begin(), pathStr.end(), '/'));

			LOGD << "Depth: " << depth;

			std::vector<Entry> matches = getMatchingEntries(db, relPath.generic(), depth + 1);

			baseEntries.insert(baseEntries.end(), matches.begin(), matches.end());

		}

		// Remove duplicates 
		sort(baseEntries.begin(), baseEntries.end(), [](const Entry& l, const Entry& r)
			{
				return l.path < r.path;
			});
		baseEntries.erase(unique(baseEntries.begin(), baseEntries.end(), [](const Entry& l, const Entry& r)
			{
				return l.path == r.path;
			}), baseEntries.end());

		// Sort by type
		sort(baseEntries.begin(), baseEntries.end(), [](const Entry& l, const Entry& r)
			{
				return l.type < r.type;
			});
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

		bool expandFolders = recursive;

		if (paths.empty()) {


			const auto currentPath = fs::current_path();
			auto root = io::Path(directory);

			// If we are inside ddb folder we can use our current path
			// otherwise we should reset to root folder
			// I.E: \home\tmp\ddb ls -w \home\img

			pathList.emplace_back(root.isParentOf(fs::current_path()) ? io::Path(currentPath).generic() : directory.string());

		}
		else
			pathList = std::vector<fs::path>(paths.begin(), paths.end());


		std::vector<Entry> baseEntries;

		getBaseEntries(db, pathList, directory, expandFolders, baseEntries);

		const bool isSingle = pathList.size() == baseEntries.size();

		LOGD << "Expand folders? " << expandFolders;

		std::vector<Entry> outputEntries;

		for (const Entry& entry : baseEntries) {
			if (entry.type != Directory)
				outputEntries.emplace_back(Entry(entry));
			else {

				if (!isSingle || !expandFolders)
					outputEntries.emplace_back(Entry(entry));

				if (expandFolders) {
					/*if (format == "text" && !isSingle && !recursive)
						output << std::endl << entry.path << ":" << std::endl;*/

					const auto depth = recursive ? maxRecursionDepth : entry.depth + 2;

					std::vector<Entry> entries = getMatchingEntries(db, entry.path, depth, true);

					for (const Entry& e : entries)
						outputEntries.emplace_back(Entry(e));
				}


			}

		}

		// Sort by path
		std::sort(outputEntries.begin(), outputEntries.end(), [](const Entry& l, const Entry& r)
			{
				return l.path < r.path;
			});


		displayEntries(outputEntries, output, format);


	}

}
