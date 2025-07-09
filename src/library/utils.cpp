/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "utils.h"

#include <gdal_inc.h>
#include <sqlite3.h>
#include <spatialite.h>
#include <proj.h>

#include <pdal/pdal_features.hpp>
#include <random>

namespace ddb::utils {

std::string getPass(const std::string& prompt) {
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

    GetConsoleMode(hIn, &con_mode);
    SetConsoleMode(hIn, con_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));

    while (ReadConsoleA(hIn, &ch, 1, &dwRead, NULL) && ch != RETURN) {
        if (ch == BACKSPACE) {
            if (password.length() != 0) {
                password.resize(password.length() - 1);
            }
        } else {
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
    if (fgets(password, sizeof(password), stdin) == nullptr)
        return "";
    password[strlen(password) - 1] = 0;

    /* restore terminal */
    if (tcsetattr(fileno(stdin), TCSANOW, &oflags) != 0) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }

    return std::string(password);
#endif
}

std::string getPrompt(const std::string& prompt) {
    char input[1024];

    std::cout << prompt;
    std::cout.flush();

    if (fgets(input, sizeof(input), stdin) == nullptr)
        return "";
    input[strlen(input) - 1] = 0;

    return std::string(input);
}

time_t currentUnixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void stringReplace(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // In case 'to' contains 'from', like
                                   // replacing 'x' with 'yx'
    }
}

void sleep(int msecs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(msecs));
}

const char* charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUWXYZ0123456789";

// https://stackoverflow.com/a/50556436
DDB_DLL std::string generateRandomString(int length) {
    std::random_device rd;
    std::mt19937 generator(rd());
    const int len = static_cast<int>(strlen(charset));
    std::uniform_int_distribution<int> distribution{0, len - 1};

    std::string str(length, '\0');
    for (auto& dis : str)
        dis = charset[distribution(generator)];

    return str;
}

/// <summary>
/// Joins a vector of strings
/// </summary>
/// <param name="vec"></param>
/// <param name="separator"></param>
/// <returns></returns>
std::string join(const std::vector<std::string>& vec, char separator) {
    std::stringstream ss;
    for (const auto& s : vec)
        ss << s << separator;

    auto paths = ss.str();

    // Remove last comma
    paths.pop_back();

    return paths;
}

// Checks if path contains . or ..
DDB_DLL bool hasDotNotation(const std::string& path) {
    std::stringstream stream(path);
    std::string seg;

    while (getline(stream, seg, '/'))
        if (seg == ".." || seg == ".")
            return true;

    return false;
}

DDB_DLL bool isLowerCase(const std::string& str) {
    for (size_t i = 0; i < str.length(); i++) {
        if (std::isupper(str[i]))
            return false;
    }
    return true;
}

DDB_DLL bool isNetworkPath(const std::string& path) {
    return path.find("http://") == 0 || path.find("https://") == 0;
}

cpr::Response downloadToFile(const std::string& url,
                             const std::string& filePath,
                             bool throwOnError,
                             bool verifySsl) {
    auto session = cpr::Session();
    session.SetUrl(cpr::Url{url});
    session.SetVerifySsl(verifySsl);
    session.SetTimeout(15000);
    session.SetHeader(cpr::Header{{"Accept-Encoding", "gzip"}});

    if (fs::exists(filePath))
        fs::remove(filePath);

    std::ofstream out(filePath, std::ios::binary);

    auto response = session.Download(out);
    out.close(); // Ensure file is closed before checking size

    // Check if download was successful and file has content
    if (response.status_code != 200 || !fs::exists(filePath) || fs::file_size(filePath) == 0) {
        // Remove empty or failed download file
        if (fs::exists(filePath))
            fs::remove(filePath);

        if (throwOnError)
            throw ddb::NetException("Failed to fetch data from " + url + " (status: " + std::to_string(response.status_code) + ")");
    }

    return response;
}

std::string readFile(const std::string& address, bool throwOnError, bool verifySsl) {
    // Check if address is a local path or an url
    if (fs::exists(address)) {
        std::ifstream t(address);
        std::string str((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        return str;
    }

    auto session = cpr::Session();
    session.SetUrl(cpr::Url{address});
    session.SetVerifySsl(verifySsl);
    session.SetTimeout(10000);

    auto response = session.Download(
        cpr::WriteCallback{[](const std::string_view&, intptr_t) -> bool { return true; }, 0});

    if (response.status_code != 200 && throwOnError) {
        throw ddb::NetException("Failed to fetch data from " + address);
    }

    return response.text;
}

cpr::Header authHeader(const std::string& token) {
    return cpr::Header{{"Authorization", "Bearer " + token}};
}

cpr::Header authCookie(const std::string& token) {
    return cpr::Header{{"Cookie", "jwtToken=" + token}};
}

std::string getBuildInfo() {
    std::ostringstream ss;

#ifdef DEBUG
    ss << "Debug";
#else
    ss << "Release";
#endif

    ss << " build";

#ifdef __GNUC__
    ss << " (GCC " << __GNUC__ << "." << __GNUC_MINOR__ << ")";
#elif defined(_MSC_VER)
    ss << " (MSVC " << _MSC_VER << ")";
#endif

    ss << " compiled " << __DATE__ << " " << __TIME__;

    return ss.str();
}

DDB_DLL void printVersions() {

    LOGV << "DDB v" << APP_VERSION;
    LOGD << "Build info: " << getBuildInfo();

    LOGV << "SQLite: " << sqlite3_libversion();
    LOGV << "SpatiaLite: " << spatialite_version();
    LOGV << "GDAL: " << GDALVersionInfo("RELEASE_NAME");
    LOGV << "CURL: " << curl_version();
    LOGV << "PDAL: " << pdal::pdalVersion;

    auto pj_info = proj_info();
    LOGV << "PROJ: " << std::string(pj_info.release) + " (search path: " + pj_info.searchpath + ")";

    LOGV << "PROJ_LIB = " << getenv("PROJ_LIB");
    LOGV << "GDAL_DATA = " << getenv("GDAL_DATA");
    LOGV << "PROJ_DATA = " << getenv("PROJ_DATA");

    LOGD << "Current locale (LC_ALL): " << std::setlocale(LC_ALL, nullptr);
    LOGD << "Current locale (LC_CTYPE): " << std::setlocale(LC_CTYPE, nullptr);
    LOGD << "LC_ALL env var: " << std::getenv("LC_ALL");

}

DDB_DLL bool isNullOrEmptyOrWhitespace(const char* str, size_t maxLength) {
    if (str == nullptr)
        return true;

    // Determine length to check (either the actual string length or maxLength)
    size_t len = 0;
    if (maxLength > 0) {
        while (str[len] != '\0' && len < maxLength)
            ++len;
    } else
        len = std::strlen(str);


    if (len == 0)
        return true;

    for (size_t i = 0; i < len; ++i)
        if (!std::isspace(static_cast<unsigned char>(str[i])))
            return false;

    return true;
}

DDB_DLL bool isNullOrEmptyOrWhitespace(const char** strlist, int count, size_t maxLength) {
    if (strlist == nullptr)
        return true;

    if (count == 0)
        return true;

    for (auto strIdx = 0; strIdx < count; strIdx++) {
        if (isNullOrEmptyOrWhitespace(strlist[strIdx], maxLength))
            return true;
    }

    return false;
}

// Validate that no string in an array is null
DDB_DLL bool hasNullStringInArray(const char** strlist, int count) {
    if (strlist == nullptr)
        return true;
    if (count < 0)
        return true;

    for (int i = 0; i < count; i++) {
        if (strlist[i] == nullptr)
            return true;
    }
    return false;
}

DDB_DLL bool isValidArrayParam(const char** array, int count) {
    if (array == nullptr && count > 0)
        return false;
    if (count < 0)
        return false;
    return true;
}

// Validate string parameter allowing empty but not null
DDB_DLL bool isValidStringParam(const char* str) {
    return str != nullptr;
}

// Validate string parameter requiring non-empty
DDB_DLL bool isValidNonEmptyStringParam(const char* str) {
    return str != nullptr && strlen(str) > 0;
}


}  // namespace ddb::utils
