/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "request.h"
#include "exceptions.h"
#include "logger.h"
#include "version.h"

namespace ddb::net{

Request::Request(const std::string &url, ReqType reqType) : url(url), reqType(reqType), curl(nullptr){
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
}

Request& Request::setVerifySSL(bool flag) {
	//VERIFYPEER basically makes sure the certificate itself is valid (i.e.,
	// signed by a trusted CA, the certificate chain is complete, etc).
	// VERIFYHOST checks that the host you're talking to is the host named in
	//	the certificate.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, flag);

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

void Request::perform(Response &res){
    CURLcode ret = curl_easy_perform(curl);
    if (ret != CURLE_OK){
        std::string err(curl_easy_strerror(ret));
        throw NetException(err + ": " + errorMsg);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.statusCode);
}

}

