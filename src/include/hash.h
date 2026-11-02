/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HASH_H
#define HASH_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>


namespace ddb {

// SHA-256 over data given in pieces. OpenSSL stays out of this header: the context is owned by
// an implementation in hash.cpp. Every OpenSSL failure throws (AppException).
class Sha256 {
   public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(std::span<const unsigned char> data);
    void update(std::string_view text);

    // The digest as 64 lowercase hex characters. The object is not usable afterwards.
    std::string finishHex();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Hash {
   public:
    // SHA-256 of the content of a file (UTF-8 path); throws FSException when it cannot be opened
    static std::string fileSHA256(const std::string& path);
    static std::string strSHA256(const std::string& str);

    // CRC-64 of the bytes (the table is the Jones polynomial, as used by Redis), as hex without
    // leading zeros: names of thumbnails and cache entries, not an integrity check
    static std::string strCRC64(std::string_view bytes);
};

}  // namespace ddb

#endif  // HASH_H
