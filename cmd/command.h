/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#ifndef COMMAND_H
#define COMMAND_H

#include "../vendor/cxxopts.hpp"

namespace cmd {

class Command{
private:
    std::string help;
protected:
    virtual void run(cxxopts::ParseResult &opts) = 0;
    virtual void setOptions(cxxopts::Options &opts) = 0;
    void printHelp();
    virtual std::string description(){ return ""; }
public:
    Command();
    void run(int argc, char* argv[]);
};

}

#endif // COMMAND_H
