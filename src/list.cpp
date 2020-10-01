/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <mio.h>

#include "dbops.h"
#include "exceptions.h"

namespace ddb {

void listIndex(Database* db, const std::string &input) {
	
	const fs::path directory = rootDirectory(db);

	std::cout << "Root: " << directory << std::endl;
	auto path = io::Path(input);
	std::cout << "Path: " << path.string() << std::endl;

	const io::Path relPath = path.relativeTo(directory);
	
	std::cout << "Listing: " << input << std::endl;
	std::cout << "Rel path: " << relPath.string() << std::endl;
}

}
