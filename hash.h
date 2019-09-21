#ifndef HASH_H
#define HASH_H

#include <string>
#include <fstream>
#include "libs/hash-library/sha256.h"

class Hash{
public:
    static std::string ingestFile(const std::string &path);
};

#endif // HASH_H
