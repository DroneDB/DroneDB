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

std::string ShareService::share(const std::vector<std::string> &input, const std::string &tag, const std::string &password, bool recursive, const std::string &cwd){
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
            .authToken(authToken)
            .send();
    if (res.status() != 200) handleError(res);

    json j = res.getJSON();
    if (!j.contains("token")) handleError(res);
    std::string batchToken = j["token"];

    // TODO: multithreaded share/upload

    io::Path wd = io::Path(fs::path(cwd));

    for (auto &fp : filePaths){
        io::Path p = io::Path(fp);
        if (p.isAbsolute() && !wd.isParentOf(p.get())){
            p = p.withoutRoot();
        }else{
            p = p.relativeTo(wd.get());
        }

        LOGD << "Uploading " << p.string();

        net::Response res = net::POST(reg.getUrl("/share/upload/" + batchToken))
                .multiPartFormData({"file", fp.string()},
                                   {"path", p.generic()})
                .authToken(authToken)
                .progressCb([&p](float progress){
                    std::cout << p.generic() << ": " << progress << "%" << std::endl;
                    return true;
                })
                .maximumUploadSpeed(1024*1024)
                .send();
        if (res.status() != 200) handleError(res);

        // TODO: handle retries

        json j = res.getJSON();
        if (!j.contains("hash")) handleError(res);
        // TODO: validate sha256 hash
    }

    // Commit
    res = net::POST(reg.getUrl("/share/commit/" + batchToken))
            .authToken(authToken)
            .send();
    if (res.status() != 200) handleError(res);

    j = res.getJSON();
    if (!j.contains("objectsCount")) handleError(res);

    if (j["objectsCount"].get<unsigned long>() != filePaths.size()){
        throw RegistryException("Could not upload all files (only " +
                                std::to_string(j["objectsCount"].get<unsigned long>()) +
                                " out of " +
                                std::to_string(filePaths.size()) +
                                " succeeded)");
    }

    return reg.getUrl("/r/" + tc.tagWithoutUrl());
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
