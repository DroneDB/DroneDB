/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef RESCAN_H
#define RESCAN_H

#include "command.h"

namespace cmd
{

    class Rescan : public Command
    {
    public:
        Rescan() {}

        virtual void run(cxxopts::ParseResult &opts) override;
        virtual void setOptions(cxxopts::Options &opts) override;
        virtual std::string description() override;
        virtual std::string extendedDescription() override;
    };

}

#endif // RESCAN_H
