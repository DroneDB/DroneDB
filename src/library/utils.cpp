/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "utils.h"

#include <random>
#include <sqlite3.h>
#include <spatialite.h>
#include <gdal_inc.h>
#include <pdal/pdal_features.hpp>

namespace ddb::utils
{

    std::string getPass(const std::string &prompt)
    {
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

        while (ReadConsoleA(hIn, &ch, 1, &dwRead, NULL) && ch != RETURN)
        {
            if (ch == BACKSPACE)
            {
                if (password.length() != 0)
                {
                    password.resize(password.length() - 1);
                }
            }
            else
            {
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

        if (tcsetattr(fileno(stdin), TCSANOW, &nflags) != 0)
        {
            perror("tcsetattr");
            exit(EXIT_FAILURE);
        }

        std::cout << prompt;
        std::cout.flush();
        if (fgets(password, sizeof(password), stdin) == nullptr)
            return "";
        password[strlen(password) - 1] = 0;

        /* restore terminal */
        if (tcsetattr(fileno(stdin), TCSANOW, &oflags) != 0)
        {
            perror("tcsetattr");
            exit(EXIT_FAILURE);
        }

        return std::string(password);
#endif
    }

    std::string getPrompt(const std::string &prompt)
    {
        char input[1024];

        std::cout << prompt;
        std::cout.flush();

        if (fgets(input, sizeof(input), stdin) == nullptr)
            return "";
        input[strlen(input) - 1] = 0;

        return std::string(input);
    }

    time_t currentUnixTimestamp()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    void stringReplace(std::string &str, const std::string &from,
                       const std::string &to)
    {
        if (from.empty())
            return;
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos)
        {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length(); // In case 'to' contains 'from', like
                                      // replacing 'x' with 'yx'
        }
    }

    void sleep(int msecs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(msecs));
    }

    const char *charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUWXYZ0123456789";

    // https://stackoverflow.com/a/50556436
    DDB_DLL std::string generateRandomString(int length)
    {
        std::random_device rd;
        std::mt19937 generator(rd());
        const int len = static_cast<int>(strlen(charset));
        std::uniform_int_distribution<int> distribution{0, len - 1};

        std::string str(length, '\0');
        for (auto &dis : str)
            dis = charset[distribution(generator)];

        return str;
    }

    /// <summary>
    /// Joins a vector of strings
    /// </summary>
    /// <param name="vec"></param>
    /// <param name="separator"></param>
    /// <returns></returns>
    std::string join(const std::vector<std::string> &vec, char separator)
    {
        std::stringstream ss;
        for (const auto &s : vec)
            ss << s << separator;

        auto paths = ss.str();

        // Remove last comma
        paths.pop_back();

        return paths;
    }

    // Checks if path contains . or ..
    DDB_DLL bool hasDotNotation(const std::string &path)
    {

        std::stringstream stream(path);
        std::string seg;

        while (getline(stream, seg, '/'))
            if (seg == ".." || seg == ".")
                return true;

        return false;
    }

    DDB_DLL bool isLowerCase(const std::string &str)
    {
        for (size_t i = 0; i < str.length(); i++)
        {
            if (std::isupper(str[i]))
                return false;
        }
        return true;
    }

    DDB_DLL bool isNetworkPath(const std::string &path)
    {
        return path.find("http://") == 0 || path.find("https://") == 0;
    }

    cpr::Response downloadToFile(const std::string &url, const std::string &filePath, bool throwOnError, bool verifySsl)
    {
        auto session = cpr::Session();
        session.SetUrl(cpr::Url{url});
        session.SetVerifySsl(verifySsl);
        session.SetTimeout(10000);
        session.SetHeader(cpr::Header{{"Accept-Encoding", "gzip"}});

        if (fs::exists(filePath))
            fs::remove(filePath);

        std::ofstream out(filePath, std::ios::binary);

        auto response = session.Download(out);

        if (response.status_code != 200 && throwOnError)
        {
            throw ddb::NetException("Failed to fetch data from " + url);
        }

        return response;
    }

    std::string readFile(const std::string &address, bool throwOnError, bool verifySsl)
    {

        // Check if address is a local path or an url
        if (fs::exists(address))
        {
            std::ifstream t(address);
            std::string str((std::istreambuf_iterator<char>(t)),
                            std::istreambuf_iterator<char>());
            return str;
        }

        auto session = cpr::Session();
        session.SetUrl(cpr::Url{address});
        session.SetVerifySsl(verifySsl);
        session.SetTimeout(10000);

        auto response = session.Download(cpr::WriteCallback{[](const std::string_view &, intptr_t) -> bool
                                                            { return true; }, 0});

        if (response.status_code != 200 && throwOnError)
        {
            throw ddb::NetException("Failed to fetch data from " + address);
        }

        return response.text;
    }

    cpr::Header authHeader(const std::string &token)
    {
        return cpr::Header{{"Authorization", "Bearer " + token}};
    }

    cpr::Header authCookie(const std::string &token)
    {
        return cpr::Header{{"Cookie", "jwtToken=" + token}};
    }

    DDB_DLL std::map<std::string, std::string> getSubsystems()
    {
        std::map<std::string, std::string> subsystems;
        subsystems["SQLite"] = sqlite3_libversion();
        subsystems["SpatiaLite"] = spatialite_version();
        subsystems["GDAL"] = GDALVersionInfo("RELEASE_NAME");
        subsystems["CURL"] = curl_version();
        subsystems["PDAL"] = pdal::pdalVersion;

        return subsystems;
    }

    DDB_DLL bool isNullOrEmptyOrWhitespace(const char *str, size_t maxLength)
    {
        if (str == nullptr)
            return true;

        // Determine length to check (either the actual string length or maxLength)
        size_t len = 0;
        if (maxLength > 0) {
            while (str[len] != '\0' && len < maxLength) 
                len++;            
        } else 
            len = strlen(str);
        

        if (len == 0)
            return true;

        for (auto i = 0; i < len; i++)        
            if (!isspace(str[i]))
                return false;        

        return true;
    }

    DDB_DLL bool isNullOrEmptyOrWhitespace(const char **strlist, int count, size_t maxLength)
    {
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

} // namespace ddb::utils
