/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BM3D_H
#define BM3D_H

#include "command.h"

namespace cmd {

class Bm3d : public Command {
public:
    Bm3d() {}

    virtual int run(cxxopts::ParseResult& opts) override;
    virtual void setOptions(cxxopts::Options& opts) override;
    virtual std::string description() override;
};
}  // namespace cmd

#endif  // BM3D_H
