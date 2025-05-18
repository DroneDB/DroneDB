/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef STAMP_H
#define STAMP_H

#include "command.h"

namespace cmd
{

  class Stamp : public Command
  {
  public:
    Stamp() {}

    virtual void run(cxxopts::ParseResult &opts) override;
    virtual void setOptions(cxxopts::Options &opts) override;
    virtual std::string description() override;
  };

}

#endif // STAMP_H
