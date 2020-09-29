/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "mio.h"
#include "utils.h"

namespace ddb{
namespace io{

bool Path::checkExtension(const std::initializer_list<std::string>& matches) {
    std::string ext = p.extension().string();
    if (ext.size() < 1) return false;
    std::string extLowerCase = ext.substr(1, ext.length());
    utils::toLower(extLowerCase);

    for (auto &m : matches) {
		std::string ext = m;
		utils::toLower(ext);
        if (ext == extLowerCase) return true;
    }
    return false;
}

time_t Path::getModifiedTime() {
#ifdef WIN32
    HANDLE hFile;
    FILETIME ftModified;
    hFile = CreateFile(p.string().c_str(), 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hFile == INVALID_HANDLE_VALUE){
        throw FSException("Cannot stat mtime (open) " + p.string() + " (errcode: " + std::to_string(GetLastError()) + ")");
    }

    if (!GetFileTime(hFile, NULL, NULL, &ftModified)){
        CloseHandle(hFile);
        throw FSException("Cannot stat mtime (get time) " + p.string());
    }

    CloseHandle(hFile);
    
    //Get the number of seconds since January 1, 1970 12:00am UTC
    LARGE_INTEGER li;
    li.LowPart = ftModified.dwLowDateTime;
    li.HighPart = ftModified.dwHighDateTime;

    const int64_t UNIX_TIME_START = 0x019DB1DED53E8000; //January 1, 1970 (start of Unix epoch) in "ticks"
    const int64_t TICKS_PER_SECOND = 10000000; //a tick is 100ns

    //Convert ticks since 1/1/1970 into seconds
    return (li.QuadPart - UNIX_TIME_START) / TICKS_PER_SECOND;
#else
    struct stat result;
    if(stat(p.string().c_str(), &result) == 0) {
        return result.st_mtime;
    } else {
        throw FSException("Cannot stat mtime " + p.string());
    }
#endif
}

std::uintmax_t Path::getSize() {
#ifdef WIN32
    HANDLE hFile;
    hFile = CreateFile(p.string().c_str(), 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw FSException("Cannot stat size (open) " + p.string() + " (errcode: " + std::to_string(GetLastError()));
    }
    
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size)){
        CloseHandle(hFile);
        throw FSException("Cannot stat size (getfilesize) " + p.string());
    }
    CloseHandle(hFile);
    return size.QuadPart;
#else
    struct stat result;
    if(stat(p.string().c_str(), &result) == 0) {
        return result.st_size;
    } else {
        throw FSException("Cannot stat size " + p.string());
    }
#endif
}

bool Path::hasChildren(const std::vector<std::string> &childPaths) {
    std::string absP = fs::weakly_canonical(fs::absolute(p)).string();
    if (absP.length() > 1 && absP.back() == fs::path::preferred_separator) absP.pop_back();

    for (auto &cp : childPaths) {
        std::string absC = fs::weakly_canonical(fs::absolute(cp)).string();
        if (absC.length() > 1 && absC.back() == fs::path::preferred_separator) absC.pop_back();
        if (absC.rfind(absP, 0) != 0 || absP == absC) return false;
    }

    return true;
}

bool Path::isParentOf(const fs::path &childPath){
    std::string absP = fs::weakly_canonical(fs::absolute(p)).string();
    if (absP.length() > 1 && absP.back() == fs::path::preferred_separator) absP.pop_back();
    std::string absC = fs::weakly_canonical(fs::absolute(childPath)).string();
    if (absC.length() > 1 && absC.back() == fs::path::preferred_separator) absC.pop_back();
    return absC.rfind(absP, 0) == 0 && absP != absC;
}

bool Path::isAbsolute() const{
    return p.is_absolute();
}

bool Path::isRelative() const{
    return p.is_relative();
}




// Counts the number of path components
// it does NOT normalize the path to account for ".." and "." folders
int Path::depth() {
    int count = 0;
    for (auto &it : p) {
        if (it.c_str()[0] != fs::path::preferred_separator &&
            it.string() != fs::current_path().root_name()) count++;
    }
    return std::max(count - 1, 0);
}

Path Path::relativeTo(const fs::path &parent){
    // Special case where parent == path
    if (fs::weakly_canonical(fs::absolute(p)) == fs::weakly_canonical(fs::absolute(parent))) {
        return fs::path("");
    }

    // Handle special cases where root is "/"
    // in this case we return the relative canonical absolute path
    if (parent == parent.root_path() || parent == "/"){
        return fs::weakly_canonical(fs::absolute(p)).relative_path();
    }

    fs::path relPath = fs::relative(fs::weakly_canonical(fs::absolute(p)), fs::weakly_canonical(fs::absolute(parent)));
    if (relPath.generic_string() == ".") return fs::weakly_canonical(fs::absolute(parent));
    else return relPath;
}


Path Path::withoutRoot(){
    if (!isAbsolute()) return Path(p);
    return Path(p.relative_path());
}

std::string Path::generic() const{
    std::string res = p.generic_string();

    // Remove trailing slash from directory (unless "/")
    if (res.length() > 1 && res[res.length() - 1] == '/') res.pop_back();

    return res;
}

std::string Path::string() const{
    return p.string();
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

    fs::path exePath = getExeFolderPath();
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

    #ifdef WIN32
    // Check same location as DLL
    char dllPath[MAX_PATH];
    HMODULE hm = NULL;

    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR) &getDataPath, &hm) == 0){
        int ret = GetLastError();
        LOGD << "GetModuleHandle failed, error = " << ret;
        return "";
    }

    if (GetModuleFileName(hm, dllPath, sizeof(dllPath)) == 0){
        int ret = GetLastError();
        LOGD << "GetModuleFileName failed, error = " << ret;
        return "";
    }

    fs::path dllDir = (fs::path(dllPath)).parent_path();
    if (fs::exists(dllDir / p)){
        return dllDir / p;
    }
    #else
    // On *nix, check /usr/share /usr/local/share folders
    fs::path usrLocalShare = fs::path("/usr/local/share/ddb");
    if (fs::exists(usrLocalShare / p)){
        return usrLocalShare / p;
    }

    fs::path usrShare = fs::path("/usr/share/ddb");
    if (fs::exists(usrShare / p)){
        return usrShare / p;
    }
    #endif

    return "";
}

fs::path getCwd(){
    char result[PATH_MAX];
#ifdef WIN32
    if (_getcwd(result, PATH_MAX) == NULL) throw FSException("Cannot get cwd");
#else
    if (getcwd(result, PATH_MAX) == NULL) throw FSException("Cannot get cwd");
#endif
    return fs::path(result);
}

std::string bytesToHuman(std::uintmax_t bytes){
    std::ostringstream os;

    const char* suffixes[7];
    suffixes[0] = "B";
    suffixes[1] = "KB";
    suffixes[2] = "MB";
    suffixes[3] = "GB";
    suffixes[4] = "TB";
    suffixes[5] = "PB";
    suffixes[6] = "EB";
    std::uintmax_t s = 0;

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
}
