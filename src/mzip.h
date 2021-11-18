/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MZIP_H
#define MZIP_H

#include <string>
#include <iostream>
#include <vector>
#include <zip.h>

#include "ddb_export.h"

namespace ddb::zip{

DDB_DLL void extractAllFromBuffer(char *zipBuffer, size_t length, const std::string &outdir, std::ostream *progressOut = nullptr);
DDB_DLL void extractAll(const std::string &zipFile, const std::string &outdir, std::ostream *progressOut = nullptr);
DDB_DLL void zipFolder(const std::string &folder, const std::string &zipFile, const std::vector<std::string> &excludes);

DDB_DLL void _extractAll(zip_t* pZip, const std::string &outdir, std::ostream *progressOut = nullptr);

}

#endif // MZIP_H
