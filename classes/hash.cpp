/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <sstream>
#include "hash.h"
#include "exceptions.h"

using namespace ddb;

std::string Hash::fileSHA256(const std::string &path) {
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

std::string Hash::strSHA256(const std::string &str){
    SHA256 digestSha2;
    digestSha2.add(str.c_str(), str.length());
    return digestSha2.getHash();
}

std::string Hash::strCRC64(const std::string &str){
    return Hash::strCRC64(str.c_str(), str.length());
}

std::string Hash::strCRC64(const char *str, uint64_t size){
    uint64_t crc = 0;
    uint64_t j;

    for (j = 0; j < size; j++) {
        uint8_t byte = str[j];
        crc = crc64_table[(uint8_t)crc ^ byte] ^ (crc >> 8);
    }

    std::ostringstream os;
    os << std::hex << crc;

    return os.str();
}


