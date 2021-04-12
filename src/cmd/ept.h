/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef EPT_CMD_H
#define EPT_CMD_H

#include "command.h"

namespace cmd {

class Ept : public Command {
  public:
    Ept() {}

    virtual void run(cxxopts::ParseResult &opts) override;
    virtual void setOptions(cxxopts::Options &opts) override;
    virtual std::string description() override;
};

}

#endif // EPT_CMD_H
