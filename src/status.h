/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STATUS_H
#define STATUS_H

#include "entry.h"
#include "ddb_export.h"

#include "dbops.h"

namespace ddb {
	enum FileStatus {
        NotIndexed,
		Deleted,
		Modified
	};

	typedef std::function<void(const FileStatus status, const std::string& file)> FileStatusCallback;

	DDB_DLL void statusIndex(Database* db, const FileStatusCallback& cb);

}

#endif // STATUS_H
