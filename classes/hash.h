#ifndef HASH_H
#define HASH_H

#include <string>
#include <fstream>
#include "../vendor/hash-library/sha256.h"

class Hash{
public:
    static std::string ingest(const std::string &path);
};

#endif // HASH_H
