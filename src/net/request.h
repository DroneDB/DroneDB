/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NET_REQUEST_H
#define NET_REQUEST_H

#include <curl/curl.h>
#include <string>
#include <vector>
#include "reqtype.h"
#include "response.h"
#include "ddb_export.h"

namespace ddb::net{

class Request{
    std::string url;
    ReqType reqType;
    CURL *curl;
    char errorMsg[CURL_ERROR_SIZE];
    struct curl_slist *headers;

    std::string urlEncode(const std::string &str);
    void perform(Response &res);
public:
    DDB_DLL Request(const std::string &url, ReqType reqType);
    DDB_DLL ~Request();

    DDB_DLL Response send();
    DDB_DLL Response downloadToFile(const std::string &outFile);

    DDB_DLL Request &formData(std::vector<std::string> params);
    DDB_DLL Request& setVerifySSL(bool flag);
    DDB_DLL Request& setAuthToken(const std::string &token);

};

}

#endif // NET_REQUEST_H
