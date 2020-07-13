/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "request.h"
#include "../exceptions.h"
#include "../../logger.h"

namespace ddb::net{

Request::Request(const std::string &url, ReqType reqType) : url(url), reqType(reqType), curl(nullptr){
    try {
        curl = curl_easy_init();
        if (!curl) throw CURLException("Cannot initialize CURL");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        if (is_logger_verbose()){
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }


    }catch (AppException &e) {
        if (curl) curl_easy_cleanup(curl);
        throw e;
    }
}

Request::~Request(){
    if (curl) curl_easy_cleanup(curl);
}

void Request::downloadToFile(const std::string &outFile){
    FILE *f = nullptr;

    f = fopen(outFile.c_str(), "wb");
    if (!f) throw FSException("Cannot open " + outFile + " for writing");

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (curl_easy_perform(curl) != CURLE_OK) throw CURLException("Cannot download " + url + ", perhaps the service is offline or unreachable.");

    fclose(f);
}

}

