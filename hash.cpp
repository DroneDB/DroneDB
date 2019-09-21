#include "hash.h"
#include "exceptions.h"

std::string Hash::ingestFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw FSException("Cannot open " + path + " for hashing");
    }

    const size_t BufferSize = 144*7*1024;
    char* buffer = new char[BufferSize];
    SHA256 digestSha2;

    while (f) {
        f.read(buffer, BufferSize);
        size_t numBytesRead = size_t(f.gcount());
        digestSha2.add(buffer, numBytesRead);
    }

    f.close();
    delete[] buffer;

    return digestSha2.getHash();
}


