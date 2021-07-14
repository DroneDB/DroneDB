#include <curl/curl.h>
#include <fstream>
#include "functions.h"
#include "exceptions.h"
#include "logger.h"
#include "request.h"
#include "utils.h"

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

std::string readFile(const std::string &url){
    if (url.find("http://") == 0 || url.find("https://") == 0){
        // Net request
        return GET(url).send().getText();
    }else{
        std::string path = url;
        utils::string_replace(path, "file://", "");
        if (!fs::exists(path)) throw FSException(path + " does not exist");

        std::ifstream i(path);
        std::string res((std::istreambuf_iterator<char>(i)),
                         std::istreambuf_iterator<char>());
        return res;
    }
}




}
