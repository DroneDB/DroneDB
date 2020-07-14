/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "request.h"
#include "../exceptions.h"
#include "../../logger.h"
#include "../../version.h"

namespace ddb::net{

Request::Request(const std::string &url, ReqType reqType) : url(url), reqType(reqType), curl(nullptr){
    try {
        curl = curl_easy_init();
        if (!curl) throw CURLException("Cannot initialize CURL");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        if (is_logger_verbose()){
            curl_easy_setopt(curl, CURLOPT_VERBOSE, true);
        }

        if (reqType == ReqType::HTTP_POST){
            curl_easy_setopt(curl, CURLOPT_POST, true);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }

        curl_easy_setopt(curl, CURLOPT_USERAGENT, "dronedb-agent/" APP_VERSION);

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
    char *encoded = curl_easy_escape(curl, str.c_str(), str.size());
    std::string s(encoded);
    curl_free(encoded);
    return s;
}

Response Request::send(){
    Response res;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Response::WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&res);

    if (curl_easy_perform(curl) != CURLE_OK) throw CURLException("Cannot complete request to " + url);

    return res;
}

Request &Request::formData(std::initializer_list<std::string> params){
    std::vector<std::string> v = params;
    if (v.size() % 2 != 0) throw CURLException("Invalid number of formData parameters");

    std::stringstream ss;
    std::cerr << "1" << std::endl;
    for (int i = 0; params.size(); i += 2){
        std::cout << v[i].c_str() << "=" << v[i + 1].c_str() << std::endl;
        ss << urlEncode(v[i]) << "=" <<
              urlEncode(v[i + 1]);
        if (i + 2 < static_cast<int>(params.size())) ss << "&";
    }
    std::cerr << "2";
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ss.str().c_str());

    return *this;
}

size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

void Request::downloadToFile(const std::string &outFile){
    FILE *f = nullptr;

    f = fopen(outFile.c_str(), "wb");
    if (!f) throw FSException("Cannot open " + outFile + " for writing");

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, true);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

    if (curl_easy_perform(curl) != CURLE_OK) throw CURLException("Cannot download " + url + ", perhaps the service is offline or unreachable.");

    fclose(f);
}

}

