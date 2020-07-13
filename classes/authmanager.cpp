#include "authmanager.h"

namespace ddb{

void AuthManager::ReadFromDisk(){
    if (fs::exists(authFile)){
        std::ifstream fin(authFile);

    }
}

void AuthManager::WriteToDisk()
{

}

AuthManager::AuthManager(const fs::path &authFile) : authFile(authFile){
    ReadFromDisk();
}

}
