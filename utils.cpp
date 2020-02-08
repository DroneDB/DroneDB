/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "utils.h"

namespace utils {

bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches) {
    std::string ext = extension.string();
    if (ext.size() < 1) return false;
    std::string extLowerCase = ext.substr(1, ext.size());
    utils::toLower(extLowerCase);

    for (auto &m : matches) {
        if (m == extLowerCase) return true;
    }
    return false;
}

time_t getModifiedTime(const std::string &filePath) {
    struct stat result;
    if(stat(filePath.c_str(), &result) == 0) {
        return result.st_mtime;
    } else {
        throw FSException("Cannot stat " + filePath);
    }
}

off_t getSize(const std::string &filePath) {
    struct stat result;
    if(stat(filePath.c_str(), &result) == 0) {
        return result.st_size;
    } else {
        throw FSException("Cannot stat " + filePath);
    }
}

bool pathsAreChildren(const fs::path &parentPath, const std::vector<std::string> &childPaths) {
    std::string absP = fs::weakly_canonical(fs::absolute(parentPath));
    if (absP.length() > 1 && absP.back() == fs::path::preferred_separator) absP.pop_back();

    for (auto &cp : childPaths) {
        std::string absC = fs::weakly_canonical(fs::absolute(cp));
        if (absC.rfind(absP, 0) != 0) return false;
    }

    return true;
}

fs::path getExeFolderPath() {
#ifdef WIN32
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return fs::path(path).parent_path();
#else
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    return fs::path(std::string(result, static_cast<size_t>((count > 0) ? count : 0))).parent_path();
#endif
}

fs::path getDataPath(const fs::path &p){
    // Attempt to find a path within the places we would
    // usually look for DATA files.
    const char *ddb_data_path = std::getenv("DDB_DATA");
    if (ddb_data_path && fs::exists(fs::path(ddb_data_path) / p)){
        return fs::path(ddb_data_path) / p;
    }

    fs::path exePath = utils::getExeFolderPath();
    if (fs::exists(exePath / p)){
        return exePath / p;
    }

    fs::path cwd = getCwd();
    if (fs::exists(cwd / "ddb_data" / p)){
        return cwd / "ddb_data" / p;
    }

    if (fs::exists(cwd / p)){
        return cwd / p;
    }

    return "";
}

fs::path getCwd(){
    char result[PATH_MAX];
#ifdef WIN32
    _getcwd(result, PATH_MAX);
#else
    getcwd(result, PATH_MAX);
#endif
    return fs::path(result);
}


// Counts the number of path components
// it does NOT normalize the path to account for ".." and "." folders
int pathDepth(const fs::path &path) {
    int count = 0;
    for (auto &it : path) {
        if (it.c_str()[0] != fs::path::preferred_separator) count++;
    }
    return std::max(count - 1, 0);
}

std::string bytesToHuman(off_t bytes){
    std::ostringstream os;

    const char* suffixes[7];
    suffixes[0] = "B";
    suffixes[1] = "KB";
    suffixes[2] = "MB";
    suffixes[3] = "GB";
    suffixes[4] = "TB";
    suffixes[5] = "PB";
    suffixes[6] = "EB";
    off_t s = 0;

    double count = bytes;
    while (count >= 1024 && s < 7){
        s++;
        count /= 1024;
    }
    if (count - floor(count) == 0.0){
        os << int(count) << " " << suffixes[s];
    }else{
        os << std::fixed << std::setprecision(2) << count << " " << suffixes[s];
    }

    return os.str();
}

}
