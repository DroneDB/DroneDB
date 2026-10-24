/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MZIP_H
#define MZIP_H

#include <string>

namespace ddb::zip
{

    // Extracts every entry of zipFile into outdir (created). An entry name that is absolute or
    // would leave outdir (Zip-Slip) throws a ZipException before anything is written for it.
    void extractAll(const std::string &zipFile, const std::string &outdir);

}

#endif // MZIP_H
