/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CURL_INC_H
#define CURL_INC_H

#include <curl/curl.h>
#include "exceptions.h"

class CURLInstance{
    CURL *curl;
public:
    CURLInstance() : curl(nullptr){
        curl = curl_easy_init();
        if(!curl) throw ddb::NetException("Cannot initialize CURL");
    }

    ~CURLInstance(){
        if (curl != nullptr){
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
    }

    CURL *get(){ return curl; }

    std::string urlEncode(const std::string &str){
        char *escapedPath = curl_easy_escape(curl, str.c_str(), str.size());
        if(escapedPath) {
            std::string ret(escapedPath);
            curl_free(escapedPath);
            return ret;
        }else{
            return "";
        }
    }
};

#endif
