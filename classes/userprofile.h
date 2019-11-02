#ifndef USERPROFILE_H
#define USERPROFILE_H

#include <filesystem>
#include "../logger.h"

namespace fs = std::filesystem;

class UserProfile{
public:
    static UserProfile* get();

    fs::path getHomeDir();
    fs::path getProfileDir();
    fs::path getProfilePath(const fs::path &p, bool createIfNeeded);
private:
    UserProfile();

    void createDir(const fs::path &p);

    static UserProfile *instance;
};

#endif // USERPROFILE_H
