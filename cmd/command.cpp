#include "command.h"
#include "build.hpp"

#include <map>

namespace cmd {

static std::map<std::string, Command> commands = {
    { "build", Build() }
};

Command::Command() {
}

}
