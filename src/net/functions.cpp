#include <curl/curl.h>
#include "functions.h"
#include "exceptions.h"
#include "logger.h"
#include "request.h"

namespace ddb::net{

void Initialize(){
    curl_global_init(CURL_GLOBAL_ALL);
}

Request GET(const std::string &url){
    return Request(url, HTTP_GET);
}

Request POST(const std::string &url){
    return Request(url, HTTP_POST);
}

// TODO: do we need to curl_global_cleanup at shutdown?
// what happens if we don't?
void Destroy() {
    curl_global_cleanup();
}




}
