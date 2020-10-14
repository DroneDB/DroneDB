/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "utils.h"

namespace ddb{
namespace utils{

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
    if (fgets(password, sizeof(password), stdin) == nullptr) return "";
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

    if (fgets(input, sizeof(input), stdin) == nullptr) return "";
    input[strlen(input) - 1] = 0;

    return std::string(input);
}

time_t currentUnixTimestamp(){
    return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
}

void string_replace(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

void sleep(int msecs){
    std::this_thread::sleep_for(std::chrono::milliseconds(msecs));
}

}
}
