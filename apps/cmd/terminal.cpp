/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "include/terminal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "exceptions.h"

#ifdef _WIN32
#include <windows.h>  // GetStdHandle, GetConsoleMode, ReadConsoleA
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace cmd::terminal {

namespace {

// What a prompt without a terminal says: stdin is a pipe, a file or /dev/null, so nobody can
// type, and the echo of a password cannot be turned off
ddb::AuthException noTerminal() {
    // Only ddb share reads the environment: push, pull and clone need a login saved first
    return ddb::AuthException(
        "no terminal to read the password: run `ddb login -u <user> -p <password>` first "
        "(ddb share also reads DDB_USERNAME and DDB_PASSWORD)");
}

// Whether stdin is a terminal (a console on Windows) somebody can type in
bool stdinIsTerminal() {
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// One line of stdin without its end of line; "" at end of input
std::string readLine() {
    char input[1024];
    if (fgets(input, sizeof(input), stdin) == nullptr)
        return "";
    std::string line(input);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}

}  // namespace

std::string getPass(const std::string& prompt) {
    if (!stdinIsTerminal())
        throw noTerminal();

#ifdef _WIN32
    const char BACKSPACE = 8;
    const char RETURN = 13;

    std::string password;
    unsigned char ch = 0;

    DWORD con_mode;
    DWORD dwRead;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    if (!GetConsoleMode(hIn, &con_mode))
        throw noTerminal();

    std::cout << prompt;
    std::cout.flush();

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

    // The echo comes back for what is typed after ddb
    SetConsoleMode(hIn, con_mode);

    std::cout << std::endl;

    return password;
#else
    struct termios oflags, nflags;

    /* disabling echo */
    if (tcgetattr(fileno(stdin), &oflags) != 0)
        throw noTerminal();
    nflags = oflags;
    nflags.c_lflag &= ~ECHO;
    nflags.c_lflag |= ECHONL;

    if (tcsetattr(fileno(stdin), TCSANOW, &nflags) != 0)
        throw noTerminal();

    std::cout << prompt;
    std::cout.flush();
    const std::string password = readLine();

    /* restore terminal, also at end of input */
    tcsetattr(fileno(stdin), TCSANOW, &oflags);

    return password;
#endif
}

std::string getPrompt(const std::string& prompt) {
    // The user name is the first half of the credentials: without a terminal, nothing is asked
    if (!stdinIsTerminal())
        throw noTerminal();

    std::cout << prompt;
    std::cout.flush();

    return readLine();
}

}  // namespace cmd::terminal
