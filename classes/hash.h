#ifndef HASH_H
#define HASH_H

#include <string>
#include <fstream>
#include "../vendor/hash-library/sha256.h"

class Hash{
public:
    static std::string ingestFile(const std::string &path);
    static std::string ingestStr(const std::string &str);
};

#endif // HASH_H
