#include "index.h"


void updateIndex(const std::string &directory, Database *db) {
    // fs::directory_options::skip_permission_denied
    for(auto i = fs::recursive_directory_iterator(directory);
            i != fs::recursive_directory_iterator();
            ++i ) {
        fs::path filename = i->path().filename();

        // Skip .ddb
        if(filename == ".ddb") i.disable_recursion_pending();

        else {
            if (checkExtension(i->path().extension(), {"jpg", "jpeg", "tif", "tiff"})) {
                std::cout << i->path() << '\n';
            }
        }
    }
}

bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches) {
    std::string ext = extension.string();
    if (ext.size() < 1) return false;
    std::string extLowerCase = ext.substr(1, ext.size());
    std::transform(extLowerCase.begin(), extLowerCase.end(), extLowerCase.begin(),
    [](unsigned char c) {
        return std::tolower(c);
    });
    for (auto &m : matches) {
        if (m == extLowerCase) return true;
    }
    return false;
}

