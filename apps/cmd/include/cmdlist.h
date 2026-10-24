/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef COMMAND_LIST_H
#define COMMAND_LIST_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "command.h"

namespace cmd
{

  // The commands of the CLI by name, with their aliases. A command is made when it is asked
  // for (commands keep no state between runs), so nothing lives in static storage.
  class CommandRegistry
  {
  public:
    using Factory = std::function<std::unique_ptr<Command>()>;

    // The commands and aliases of ddb
    static const CommandRegistry &builtin();

    // Throws std::logic_error when the name is already registered
    void add(const std::string &name, Factory factory);

    template <typename T>
    void add(const std::string &name)
    {
      add(name, [] { return std::make_unique<T>(); });
    }

    // An alias of a registered command. Throws std::logic_error when the target is not
    // registered or the name is already taken: a registry with a dangling alias never starts.
    void alias(const std::string &alias, const std::string &target);

    // The command a name stands for: the name itself, or the target of the alias. Empty when
    // there is no such command.
    std::string resolve(const std::string &name) const;

    // A new command by name or alias; null when there is none
    std::unique_ptr<Command> create(const std::string &name) const;

    // Registered command names, sorted
    std::vector<std::string> names() const;

    // alias -> command name
    const std::map<std::string, std::string> &aliases() const { return aliases_; }

  private:
    std::map<std::string, Factory> factories_;
    std::map<std::string, std::string> aliases_;
  };

}

#endif // COMMAND_LIST_H
