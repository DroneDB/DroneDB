/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SHARE_CMD_H
#define SHARE_CMD_H

#include "credentials.h"

namespace cmd
{

  class Share : public RegistryCommand
  {
  public:
    explicit Share(RegistryServices registryServices = {}) : RegistryCommand(std::move(registryServices)) {}

    virtual int run(cxxopts::ParseResult &opts) override;
    virtual void setOptions(cxxopts::Options &opts) override;
    virtual std::string description() override;
  };

}

#endif // SHARE_CMD_H
