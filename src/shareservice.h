/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SHARESERVICE_H
#define SHARESERVICE_H

#include <string>
#include <vector>
#include "net.h"
#include "ddb_export.h"

namespace ddb{

typedef std::function<bool(const std::string &filename, float progress)> ShareCallback;

class ShareService{
    void handleError(net::Response &res);
public:
    DDB_DLL ShareService();
    DDB_DLL std::string share(const std::vector<std::string> &input, const std::string &tag,
                              const std::string &password, bool recursive, const std::string &cwd,
                              const ShareCallback &cb = nullptr);
};

}
#endif // SHARESERVICE_H
