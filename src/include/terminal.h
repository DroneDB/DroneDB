/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef TERMINAL_H
#define TERMINAL_H

// Reading from the terminal (DQ-05: out of utils.h, which pulled termios.h and windows.h into
// every file). The library still prompts when push, pull and clone find no credentials;
// DQ-11 moves the prompts to apps/ (CredentialProvider) and this header with them.

#include <string>

#include "ddb_export.h"

namespace ddb::terminal {

// Prints prompt and reads one line from stdin (without the newline); "" at end of input
DDB_DLL std::string getPrompt(const std::string& prompt = "");

// Prints prompt and reads one line from stdin without echoing it (getpass, also on Windows)
DDB_DLL std::string getPass(const std::string& prompt = "Password: ");

}  // namespace ddb::terminal

#endif  // TERMINAL_H
