#ifndef NET_H
#define NET_H

#include "net/functions.h"
#include "net/request.h"

typedef std::function<bool(std::string& fileName, size_t txBytes, size_t totalBytes)> UploadCallback;


#endif // NET_H
