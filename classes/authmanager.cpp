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

}
