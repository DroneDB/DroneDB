/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef MASK_CMD_H
#define MASK_CMD_H

#include "command.h"

namespace cmd
{

  class Mask : public Command
  {
  public:
    Mask() {}

    virtual void run(cxxopts::ParseResult &opts) override;
    virtual void setOptions(cxxopts::Options &opts) override;
    virtual std::string description() override;
  };

}

#endif  // MASK_CMD_H
