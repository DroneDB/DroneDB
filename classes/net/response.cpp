/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "response.h"
#include "../exceptions.h"
#include "../../logger.h"

namespace ddb::net{

Response::Response() : statusCode(0), buf(nullptr), bufSize(0){

}

Response::~Response(){
    if (buf) {
        free(buf);
        buf = nullptr;
    }
}

char *Response::getData(){
    return buf;
}

json Response::getJSON(){
    try{
        return json::parse(getData());
    }catch(const json::parse_error &e){
        throw JSONException("Invalid JSON: " + std::string(getData()));
    }
}

long Response::status(){
    return statusCode;
}

size_t Response::WriteCallback(void *contents, size_t size, size_t nmemb, void *userp){
  size_t realsize = size * nmemb;
  Response *res = static_cast<Response *>(userp);

  char *ptr = static_cast<char *>(realloc(res->buf, res->bufSize + realsize + 1));
  if(!ptr) {
    /* out of memory! */
    LOGW << "not enough memory (realloc returned NULL)\n";
    return 0;
  }

  res->buf = ptr;
  memcpy(&(res->buf[res->bufSize]), contents, realsize);
  res->bufSize += realsize;
  res->buf[res->bufSize] = 0;

  return realsize;
}

}

