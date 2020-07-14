/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "../url.h"
#include "../utils.h"
#include "../net.h"
#include "exceptions.h"
#include "registry.h"

using homer6::Url;

namespace ddb{

Registry::Registry(const std::string &url){
    Url u;

    // Always append https if no protocol is specified
    if (url.find("https://") != 0 || url.find("http://") != 0){
        u.fromString("https://" + url);
    }else{
        u.fromString(url);
    }

    // Validate and set URL
    if (u.getScheme() != "https" && u.getScheme() != "http"){
        throw URLException("Registry URL can only be http/https");
    }

    std::string port = u.getPort() != 80 && u.getPort() != 443 ? ":" + std::to_string(u.getPort()) : "";
    this->url = u.getScheme() + "//" + u.getHost() + port + u.getPath();

}

std::string Registry::getUrl() const{
    return url;
}

std::string Registry::getToken(const std::string &username, const std::string &password) const{
	return "TODO";
}

bool Registry::login(const std::string &username, const std::string &password) const{
    net::Response res = net::POST(url)
                            .formData({"username", username, "password", password})
                            .send();
    std::cout << res.getData() << std::endl;
}

void Registry::logout(){

}

}
