/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareservice.h"
#include "registry.h"
#include "registryutils.h"
#include "fs.h"
#include "mio.h"
#include "dbops.h"
#include "userprofile.h"

namespace ddb{

ShareService::ShareService(){

}

void ShareService::share(const std::vector<std::string> &input, const std::string &tag, const std::string &password, bool recursive, const std::string &cwd){
    // TODO: add callback
    if (input.size() == 0) throw InvalidArgsException("No files to share");

    std::vector<fs::path> filePaths = getPathList(input, false, recursive ? 0 : 1);

    // Parse tag to find registry URL
    TagComponents tc = RegistryUtils::parseTag(tag);
    AuthCredentials ac = UserProfile::get()->getAuthManager()->loadCredentials(tc.registryUrl);
    if (ac.empty()) throw AuthException("No authentication credentials stored");

    Registry reg(tc.registryUrl);
    std::string authToken = reg.login(ac.username, ac.password); // Will throw error on login failed

    // Call init
    net::Response res = net::POST(reg.getUrl("/share/init"))
            .formData({"tag", tc.tagWithoutUrl(), "password", password})
            .setAuthToken(authToken)
            .send();
    if (res.status() != 200) handleError(res);

    std::cout << res.getText() << std::endl;


    // TODO: multithreaded share/upload

    io::Path wd = io::Path(fs::path(cwd));

    for (auto &fp : filePaths){
        LOGD << "Uploading " << fp.string();

        io::Path p = io::Path(fp);
        if (p.isAbsolute() && !wd.isParentOf(p.get())){
            p = p.withoutRoot();
        }else{
            p = p.relativeTo(wd.get());
        }

        LOGD << p.string();
        //        if
//        .relativeTo(cwd);

//        net::Response res = net::POST(reg.getUrl("/share/upload"))
//                .formData({"tag", tc.tagWithoutUrl(), "password", password})
//                .setAuthToken(authToken)
//                .send();
    }
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
