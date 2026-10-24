/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef TERMINAL_H
#define TERMINAL_H

// Reading from the terminal (DQ-05: out of utils.h, which pulled termios.h and windows.h into
// every file; DQ-11: out of the library, which must never prompt: a host such as the Registry
// has no stdin to ask. The credentials the library needs come from a CredentialProvider, and the
// CLI's is cmd::InteractiveCredentials).

#include <string>

namespace cmd::terminal {

// Prints prompt and reads one line from stdin (without the newline); "" at end of input
std::string getPrompt(const std::string& prompt = "");

// Prints prompt and reads one line from stdin without echoing it (getpass, also on Windows)
std::string getPass(const std::string& prompt = "Password: ");

}  // namespace cmd::terminal

#endif  // TERMINAL_H
