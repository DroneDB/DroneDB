#ifndef COMMAND_H
#define COMMAND_H

#include "../libs/cxxopts.hpp"

namespace cmd {

class Command{
public:
    Command();

    virtual void run(const cxxopts::ParseResult &opts){};
};

}

#endif // COMMAND_H
