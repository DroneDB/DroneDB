/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NET_REQUEST_H
#define NET_REQUEST_H

#include <curl/curl.h>
#include <string>
#include "reqtype.h"

namespace ddb::net{

class Request{
    std::string url;
    ReqType reqType;
    CURL *curl;
public:
    Request(const std::string &url, ReqType reqType);
    ~Request();

    void downloadToFile(const std::string &outFile);
};

}

#endif // NET_REQUEST_H
