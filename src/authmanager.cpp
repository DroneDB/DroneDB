/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <base64/base64.h>
#include "authmanager.h"
#include "logger.h"

namespace ddb{

void AuthManager::ReadFromDisk(){
    if (fs::exists(authFile)){
        std::ifstream fin(authFile);
        fin >> auth;
        LOGD << "Read " << authFile;
    }
}

void AuthManager::WriteToDisk(){
    std::ofstream fout(authFile);
    fout << auth;
    LOGD << "Wrote " << authFile;
}

AuthManager::AuthManager(const fs::path &authFile) : authFile(authFile){
    ReadFromDisk();

    // First time
    if (!auth.contains("auths")){
        LOGD << "Initializing " << authFile;
        auth["auths"] = {};
        WriteToDisk();
    }
}

void AuthManager::saveCredentials(const std::string &url, const AuthCredentials &creds){
    LOGD << "Saving credentials for " << creds.username;

    std::stringstream ss;
    ss << creds.username << ":" << creds.password;
    auth["auths"][url] = {
        { "auth", Base64::encode(ss.str()) }
    };

    WriteToDisk();
}

AuthCredentials AuthManager::loadCredentials(const std::string &url){
    AuthCredentials ac;

    if (auth["auths"].contains(url)){
        const std::string userpwd = Base64::decode(auth["auths"][url]["auth"]);
        const size_t colonpos = userpwd.rfind(":");
        if (colonpos > 0){
            LOGD << "Found username and password for " << url;
            ac.username = userpwd.substr(0, colonpos);
            ac.password = userpwd.substr(colonpos + 1, userpwd.length() - colonpos - 1);
        }
    }

    return ac;
}

bool AuthManager::deleteCredentials(const std::string &url){
    if (auth["auths"].contains(url)){
        LOGD << "Deleting credentials for " << url;
        auth["auths"].erase(url);
        WriteToDisk();
        return true;
    }

    return false;
}

std::vector<std::string> AuthManager::getAuthenticatedRegistryUrls(){
    std::vector<std::string> result;
    for (auto it = auth["auths"].begin(); it != auth["auths"].end(); it++){
        result.push_back(it.key());
    }
    return result;
}

}
