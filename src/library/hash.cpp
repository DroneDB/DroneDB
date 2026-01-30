/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <sstream>
#include <iomanip>
#include <vector>
#include "hash.h"
#include "exceptions.h"

using namespace ddb;

namespace {
    // Helper to convert hash bytes to hex string
    std::string bytesToHex(const unsigned char* hash, unsigned int len) {
        std::ostringstream oss;
        for (unsigned int i = 0; i < len; ++i) {
            oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
        }
        return oss.str();
    }
}

std::string Hash::fileSHA256(const std::string &path) {
    std::ifstream f(path, std::ios::binary);

    if (!f.is_open())
        throw FSException("Cannot open " + path + " for hashing");

    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx)
        throw AppException("Failed to create EVP_MD_CTX for SHA256");

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), NULL) != 1) {
        throw AppException("Failed to initialize SHA256 digest");
    }

    const size_t BufferSize = 144*7*1024;
    std::vector<char> buffer(BufferSize);

    while (f) {
        f.read(buffer.data(), BufferSize);
        size_t numBytesRead = size_t(f.gcount());
        if (numBytesRead > 0) {
            safeDigestUpdate(ctx.get(), buffer.data(), numBytesRead);
        }
    }

    f.close();

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen;
    safeDigestFinal(ctx.get(), hash, &hashLen);

    return bytesToHex(hash, hashLen);
}

std::string Hash::strSHA256(const std::string &str){
    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx)
        throw AppException("Failed to create EVP_MD_CTX for SHA256");

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), NULL) != 1) {
        throw AppException("Failed to initialize SHA256 digest");
    }

    safeDigestUpdate(ctx.get(), str.c_str(), str.length());

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen;
    safeDigestFinal(ctx.get(), hash, &hashLen);

    return bytesToHex(hash, hashLen);
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


