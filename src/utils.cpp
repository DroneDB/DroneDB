/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "utils.h"

namespace utils {

bool checkExtension(const fs::path &extension, const std::initializer_list<std::string>& matches) {
    std::string ext = extension.string();
    if (ext.size() < 1) return false;
    std::string extLowerCase = ext.substr(1, ext.length());
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
    std::string absP = fs::weakly_canonical(fs::absolute(parentPath)).string();
    if (absP.length() > 1 && absP.back() == fs::path::preferred_separator) absP.pop_back();

    for (auto &cp : childPaths) {
        std::string absC = fs::weakly_canonical(fs::absolute(cp)).string();
        if (absC.length() > 1 && absC.back() == fs::path::preferred_separator) absC.pop_back();
        if (absC.rfind(absP, 0) != 0 || absP == absC) return false;
    }

    return true;
}

bool pathIsChild(const fs::path &parentPath, const fs::path &p){
    std::string absP = fs::weakly_canonical(fs::absolute(parentPath)).string();
    if (absP.length() > 1 && absP.back() == fs::path::preferred_separator) absP.pop_back();
    std::string absC = fs::weakly_canonical(fs::absolute(p)).string();
    if (absC.length() > 1 && absC.back() == fs::path::preferred_separator) absC.pop_back();
    return absC.rfind(absP, 0) == 0 && absP != absC;
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
    if (_getcwd(result, PATH_MAX) == NULL) throw FSException("Cannot get cwd");
#else
    if (getcwd(result, PATH_MAX) == NULL) throw FSException("Cannot get cwd");
#endif
    return fs::path(result);
}


// Counts the number of path components
// it does NOT normalize the path to account for ".." and "." folders
int pathDepth(const fs::path &path) {
    int count = 0;
    for (auto &it : path) {
        if (it.c_str()[0] != fs::path::preferred_separator &&
			it.string() != fs::current_path().root_name()) count++;
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

std::string getPass(const std::string &prompt){
#ifdef _WIN32
    const char BACKSPACE = 8;
    const char RETURN = 13;

    std::string password;
    unsigned char ch = 0;

    std::cout << prompt;
    std::cout.flush();

    DWORD con_mode;
    DWORD dwRead;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    GetConsoleMode( hIn, &con_mode );
    SetConsoleMode( hIn, con_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT) );

    while(ReadConsoleA( hIn, &ch, 1, &dwRead, NULL) && ch != RETURN){
         if(ch == BACKSPACE){
            if(password.length() != 0){
                password.resize(password.length()-1);
            }
         }else{
             password += ch;
         }
    }

	std::cout << std::endl;

    return password;
#else
    struct termios oflags, nflags;
    char password[1024];

    /* disabling echo */
    tcgetattr(fileno(stdin), &oflags);
    nflags = oflags;
    nflags.c_lflag &= ~ECHO;
    nflags.c_lflag |= ECHONL;

    if (tcsetattr(fileno(stdin), TCSANOW, &nflags) != 0) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }

    std::cout << prompt;
    std::cout.flush();
    fgets(password, sizeof(password), stdin);
    password[strlen(password) - 1] = 0;

    /* restore terminal */
    if (tcsetattr(fileno(stdin), TCSANOW, &oflags) != 0) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }

    return std::string(password);
#endif
}

std::string getPrompt(const std::string &prompt){
    char input[1024];

    std::cout << prompt;
    std::cout.flush();

    fgets(input, sizeof(input), stdin);
    input[strlen(input) - 1] = 0;

    return std::string(input);
}

}
