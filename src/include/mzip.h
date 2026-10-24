/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MZIP_H
#define MZIP_H

#include <string>
#include <iostream>
#include <vector>
#include <zip.h>


namespace ddb::zip
{

    void extractAllFromBuffer(const void *zipBuffer, size_t length, const std::string &outdir, std::ostream *progressOut = nullptr);
    void extractAll(const std::string &zipFile, const std::string &outdir, std::ostream *progressOut = nullptr);
    void zipFolder(const std::string &folder, const std::string &zipFile, const std::vector<std::string> &excludes);

    void _extractAll(zip_t *pZip, const std::string &outdir, std::ostream *progressOut = nullptr);

}

#endif // MZIP_H
