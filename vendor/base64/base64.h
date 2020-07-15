#ifndef _BASE64_H_
#define _BASE64_H_

#include <vector>
#include <string>

class Base64
{
public:
    static std::string encode(const std::string &buf);
    static std::string encode(const std::vector<unsigned char>& buf);
    static std::string encode(const unsigned char* buf, unsigned int bufLen);
    static std::vector<uint8_t> decode_bytes(std::string encoded_string);
    static std::string decode(std::string encoded_string);
};

#endif
