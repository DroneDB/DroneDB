#ifndef NET_FUNCTIONS_H
#define NET_FUNCTIONS_H

#include <string>
#include "request.h"
#include "ddb_export.h"

namespace ddb::net{

DDB_DLL void Initialize();
DDB_DLL Request GET(const std::string &url);
DDB_DLL Request POST(const std::string &url);


}

#endif // NET_FUNCTIONS_H
