/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "request.h"
#include "exceptions.h"
#include "logger.h"
#include "version.h"

namespace ddb::net{

Request::Request(const std::string &url, ReqType reqType) :
    url(url), reqType(reqType), curl(nullptr), headers(nullptr),
    form(nullptr), cb(nullptr){
    try {
        curl = curl_easy_init();
        if (!curl) throw NetException("Cannot initialize CURL");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        if (is_logger_verbose()){
            curl_easy_setopt(curl, CURLOPT_VERBOSE, true);
        }

        if (reqType == ReqType::HTTP_POST){
            curl_easy_setopt(curl, CURLOPT_POST, true);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }

        curl_easy_setopt(curl, CURLOPT_USERAGENT, "dronedb-agent/" APP_VERSION);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMsg);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
    }catch (AppException &e) {
        if (curl) curl_easy_cleanup(curl);
        throw e;
    }
}

Request::~Request(){
    if (curl) curl_easy_cleanup(curl);
	curl = nullptr;
    if (headers) curl_slist_free_all(headers);
    headers = nullptr;
    if (form) curl_mime_free(form);
    form = nullptr;
}

Request& Request::verifySSL(bool flag) {
	//VERIFYPEER basically makes sure the certificate itself is valid (i.e.,
	// signed by a trusted CA, the certificate chain is complete, etc).
	// VERIFYHOST checks that the host you're talking to is the host named in
	//	the certificate.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, flag);

    return *this;
}

Request &Request::authToken(const std::string &token){
    header("Authorization", "Bearer " + token);
    return *this;
}

Request &Request::progressCb(const RequestCallback &cb){
    this->cb = cb;
    return *this;
}

Request &Request::maximumUploadSpeed(unsigned long bytesPerSec){
    curl_easy_setopt(curl, CURLOPT_MAX_SEND_SPEED_LARGE, (curl_off_t)bytesPerSec);
    return *this;
}

std::string Request::urlEncode(const std::string &str){
    char *encoded = curl_easy_escape(curl, str.c_str(), static_cast<int>(str.length()));
    if (!encoded) throw NetException("Cannot url encode " + str);
    std::string s(encoded);
    curl_free(encoded);
    return s;
}

Response Request::send(){
    Response res;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Response::WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&res);

    perform(res);

    return res;
}

Request &Request::formData(std::vector<std::string> params){
    if (params.size() % 2 != 0) throw NetException("Invalid number of formData parameters");

    std::stringstream ss;
    for (unsigned long i = 0; i < params.size(); i += 2){
        ss << urlEncode(params[i]) << "=" <<
              urlEncode(params[i + 1]);
        if (i + 1 < params.size()) ss << "&";
    }
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, ss.str().c_str());

    return *this;
}

Request &Request::multiPartFormData(std::vector<std::string> files, std::vector<std::string> params){
    if (files.size() % 2 != 0) throw NetException("Invalid number of multiPartFormData files");
    if (params.size() % 2 != 0) throw NetException("Invalid number of multiPartFormData parameters");

    if (!form) form = curl_mime_init(curl);
    curl_mimepart *field = NULL;

    // Expect: 100-continue is not wanted
    header("Expect:");

    // Add files
    for (unsigned long i = 0; i < files.size(); i += 2){
        field = curl_mime_addpart(form);
        curl_mime_name(field, files[i].c_str());
        curl_mime_filedata(field, files[i + 1].c_str());
    }

    // Add parameters
    for (unsigned long i = 0; i < params.size(); i += 2){
        field = curl_mime_addpart(form);
        curl_mime_name(field, params[i].c_str());
        curl_mime_data(field, params[i + 1].c_str(), CURL_ZERO_TERMINATED);
    }

    return *this;
}

Request &Request::header(const std::string &header){
    headers = curl_slist_append(headers, header.c_str());
    return *this;
}

Request &Request::header(const std::string &name, const std::string &value){
    header(name + ": " + value);
    return *this;
}

size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

Response Request::downloadToFile(const std::string &outFile){
    FILE *f = nullptr;

    f = fopen(outFile.c_str(), "wb");
    if (!f) throw FSException("Cannot open " + outFile + " for writing");

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, true);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);

    Response res;
    perform(res);
    fclose(f);

    return res;
}

static int xferinfo(void *p,
                    curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t ultotal, curl_off_t ulnow){
  struct RequestProgress *progress = static_cast<struct RequestProgress *>(p);

//  CURL *curl = static_cast<CURL *>(progress->curl);

//  CURLINFO_TOTAL_TIME_T is Unavailable in old cURL versions
//  curl_off_t curTime = 0;
//  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &curTime);

//  if((curTime - progress->lastRuntime) >= 10000) {
//    progress->lastRuntime = curTime;

  float progValue = dltotal + ultotal > 0 ?
                      static_cast<float>(dlnow + ulnow) / static_cast<float>(dltotal + ultotal) :
                    0.0f;

  if (!(*progress->cb)(progValue * 100.0f)){
      // Handle cancel
      return 1;
  }

//}

  return 0;
}

void Request::perform(Response &res){
    if (headers != nullptr){
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (form != nullptr){
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    }

    if (cb != nullptr){
        struct RequestProgress prog;
        prog.lastRuntime = 0;
        prog.curl = curl;
        prog.cb = &cb;
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
    }

    CURLcode ret = curl_easy_perform(curl);
    if (ret != CURLE_OK){
        std::string err(curl_easy_strerror(ret));
        throw NetException(err + ": " + errorMsg);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.statusCode);

    if (cb != nullptr){
        cb(100.0);
    }
}

}

