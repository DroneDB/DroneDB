/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COMMAND_LIST_H
#define COMMAND_LIST_H

#include <map>
#include <string>
#include "command.h"

namespace cmd {

  extern std::map<std::string, Command*> commands;
  extern std::map<std::string, std::string> aliases;

}

#endif // COMMAND_LIST_H
