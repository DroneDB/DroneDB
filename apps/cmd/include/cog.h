/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef COG_CMD_H
#define COG_CMD_H

#include "command.h"

namespace cmd
{

  class Cog : public Command
  {
  public:
    Cog() {}

    virtual void run(cxxopts::ParseResult &opts) override;
    virtual void setOptions(cxxopts::Options &opts) override;
    virtual std::string description() override;
  };

}

#endif // COG_CMD_H
