#include <base64/base64.h>
#include "authmanager.h"
#include "../logger.h"

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
        "auth", Base64::encode(ss.str())
    };

    WriteToDisk();
}

AuthCredentials AuthManager::loadCredentials(const std::string &url){
    AuthCredentials ac;

    if (auth["auths"].contains(url)){
        std::string userpwd = Base64::decode(auth["auths"][url]);
        int colonpos = userpwd.rfind(":");
        if (colonpos > 0){
            LOGD << "Found username and password for " << url;
            ac.username = userpwd.substr(0, colonpos - 1);
            ac.password = userpwd.substr(colonpos + 1, userpwd.length() - colonpos - 1);
        }
    }

    return ac;
}

}
