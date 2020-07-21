#ifndef NET_FUNCTIONS_H
#define NET_FUNCTIONS_H

#include <string>
#include "request.h"

namespace ddb::net{

void Initialize();
Request GET(const std::string &url);
Request POST(const std::string &url);


}

#endif // NET_FUNCTIONS_H
