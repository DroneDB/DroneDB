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

typedef std::function<bool(size_t txBytes, size_t totalBytes)> RequestCallback;

struct RequestProgress {
  curl_off_t lastRuntime;
  CURL *curl;
  RequestCallback *cb;
};


class Request{
    std::string url;
    ReqType reqType;
    CURL *curl;
    char errorMsg[CURL_ERROR_SIZE];
    struct curl_slist *headers;
    curl_mime *form;

    RequestCallback cb;

    std::string urlEncode(const std::string &str);
    void perform(Response &res);
public:
    DDB_DLL Request(const std::string &url, ReqType reqType);
    DDB_DLL ~Request();

    DDB_DLL Response send();
    DDB_DLL Response downloadToFile(const std::string &outFile);

    DDB_DLL Request& formData(std::vector<std::string> params);
    DDB_DLL Request& multiPartFormData(std::vector<std::string> files, std::vector<std::string> params = {});
    DDB_DLL Request& header(const std::string &header);
    DDB_DLL Request& header(const std::string &name, const std::string &value);
    DDB_DLL Request& verifySSL(bool flag);
    DDB_DLL Request& authToken(const std::string &token);

    DDB_DLL Request& progressCb(const RequestCallback &cb);
    DDB_DLL Request& maximumUploadSpeed(unsigned long bytesPerSec);
};

}

#endif // NET_REQUEST_H
