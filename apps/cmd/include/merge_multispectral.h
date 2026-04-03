/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef MERGE_MULTISPECTRAL_CMD_H
#define MERGE_MULTISPECTRAL_CMD_H

#include "command.h"

namespace cmd
{

  class MergeMultispectral : public Command
  {
  public:
    MergeMultispectral() {}

    virtual void run(cxxopts::ParseResult &opts) override;
    virtual void setOptions(cxxopts::Options &opts) override;
    virtual std::string description() override;
  };

}

#endif // MERGE_MULTISPECTRAL_CMD_H
