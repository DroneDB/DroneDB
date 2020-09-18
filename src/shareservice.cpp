/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareservice.h"
#include "registry.h"
#include "registryutils.h"
#include "fs.h"
#include "dbops.h"
#include "userprofile.h"

namespace ddb{

ShareService::ShareService(){

}

void ShareService::share(const std::vector<std::string> &input, const std::string &tag, const std::string &password, bool recursive){
    // TODO: add callback
    if (input.size() == 0) throw InvalidArgsException("No files to share");

    std::vector<fs::path> filePaths;
    if (recursive){
        filePaths = getPathList(input, true, 0);
    }else{
        filePaths = std::vector<fs::path>(input.begin(), input.end());
    }


    // Parse tag to find registry URL
    TagComponents tc = RegistryUtils::parseTag(tag);
    AuthCredentials ac = UserProfile::get()->getAuthManager()->loadCredentials(tc.registryUrl);
    if (ac.empty()) throw AuthException("No authentication credentials stored");

    Registry reg(tc.registryUrl);
    std::string authToken = reg.login(ac.username, ac.password); // Will throw error on login failed

    // Call init
    // TODO: ddb/share/init --> /share/init ?
    net::Response res = net::POST(reg.getUrl("/ddb/share/init"))
            .formData({"tag", tc.tagWithoutUrl(), "password", password})
            .setAuthToken(authToken)
            .send();
    if (res.status() != 200) handleError(res);

    std::cout << res.getText() << std::endl;


    // TODO: ddb/share/upload
    // multithreaded
}

void ShareService::handleError(net::Response &res){
    if (res.hasData()){
        LOGD << "Request error: " << res.getText();

        auto j = res.getJSON();
        if (j.contains("error")) throw RegistryException("Error response from registry: " + j["error"].get<std::string>());
        else throw RegistryException("Invalid response from registry: " + res.getText());
    }else{
        LOGD << "Request error: " << res.status();

        throw RegistryException("Invalid response from registry. Returned status: " + std::to_string(res.status()));
    }
}

}
