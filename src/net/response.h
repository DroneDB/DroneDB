/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NET_RESPONSE_H
#define NET_RESPONSE_H

#include <curl/curl.h>
#include <string>
#include "json.h"

namespace ddb::net{

class Response{
    long statusCode;

    char *buf;
    size_t bufSize;
public:
    Response();
    Response(Response&& other) noexcept;
    Response& operator=(Response&& other) noexcept;
    ~Response();

    char* getData();
    bool hasData();
    json getJSON();
    long status();

    // Use by curl to write result into memory
    static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp);

    friend class Request;
};



}

#endif // NET_RESPONSE_H
