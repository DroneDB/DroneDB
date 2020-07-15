// Modified from https://stackoverflow.com/questions/180947/base64-decode-snippet-in-c
#include "base64.h"

static const uint8_t from_base64[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255,  62, 255,  63, 
                                     52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255, 
                                    255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
                                     15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,  63,
                                    255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40, 
                                     41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255};

static const char to_base64[] = 
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";


std::string Base64::encode(const std::string &buf){
    std::vector<uint8_t> vec(buf.begin(), buf.end());
    return Base64::encode(vec);
}

std::string Base64::encode(const std::vector<uint8_t>& buf)
{
    if (buf.empty())
        return ""; // Avoid dereferencing buf if it's empty
    return encode(&buf[0], (unsigned int)buf.size());
}

std::string Base64::encode(const uint8_t* buf, unsigned int bufLen)
{
    // Calculate how many uint8_ts that needs to be added to get a multiple of 3
    size_t missing = 0;
    size_t ret_size = bufLen;
    while ((ret_size % 3) != 0)
    {
        ++ret_size;
        ++missing;
    }

    // Expand the return string size to a multiple of 4
    ret_size = 4*ret_size/3;

    std::string ret;
    ret.reserve(ret_size);

    for (unsigned int i=0; i<ret_size/4; ++i)
    {
        // Read a group of three uint8_ts (avoid buffer overrun by replacing with 0)
        size_t index = i*3;
        uint8_t b3[3];
        b3[0] = (index+0 < bufLen) ? buf[index+0] : 0;
        b3[1] = (index+1 < bufLen) ? buf[index+1] : 0;
        b3[2] = (index+2 < bufLen) ? buf[index+2] : 0;

        // Transform into four base 64 characters
        uint8_t b4[4];
        b4[0] =                         ((b3[0] & 0xfc) >> 2);
        b4[1] = ((b3[0] & 0x03) << 4) + ((b3[1] & 0xf0) >> 4);
        b4[2] = ((b3[1] & 0x0f) << 2) + ((b3[2] & 0xc0) >> 6);
        b4[3] = ((b3[2] & 0x3f) << 0);

        // Add the base 64 characters to the return value
        ret.push_back(to_base64[b4[0]]);
        ret.push_back(to_base64[b4[1]]);
        ret.push_back(to_base64[b4[2]]);
        ret.push_back(to_base64[b4[3]]);
    }

    // Replace data that is invalid (always as many as there are missing uint8_ts)
    for (size_t i=0; i<missing; ++i)
        ret[ret_size - i - 1] = '=';

    return ret;
}

std::string Base64::decode(std::string encoded_string){
    std::vector<uint8_t> chars = decode_bytes(encoded_string);
    return std::string(chars.begin(), chars.end());
}

std::vector<uint8_t> Base64::decode_bytes(std::string encoded_string)
{
    // Make sure string length is a multiple of 4
    while ((encoded_string.size() % 4) != 0)
        encoded_string.push_back('=');

    size_t encoded_size = encoded_string.size();
    std::vector<uint8_t> ret;
    ret.reserve(3*encoded_size/4);

    for (size_t i=0; i<encoded_size; i += 4){
        // Get values for each group of four base 64 characters
        uint8_t b4[4];
        b4[0] = (encoded_string[i+0] <= 'z') ? from_base64[static_cast<uint8_t>(encoded_string[i+0])] : 0xff;
        b4[1] = (encoded_string[i+1] <= 'z') ? from_base64[static_cast<uint8_t>(encoded_string[i+1])] : 0xff;
        b4[2] = (encoded_string[i+2] <= 'z') ? from_base64[static_cast<uint8_t>(encoded_string[i+2])] : 0xff;
        b4[3] = (encoded_string[i+3] <= 'z') ? from_base64[static_cast<uint8_t>(encoded_string[i+3])] : 0xff;

        // Transform into a group of three uint8_ts
        uint8_t b3[3];
        b3[0] = ((b4[0] & 0x3f) << 2) + ((b4[1] & 0x30) >> 4);
        b3[1] = ((b4[1] & 0x0f) << 4) + ((b4[2] & 0x3c) >> 2);
        b3[2] = ((b4[2] & 0x03) << 6) + ((b4[3] & 0x3f) >> 0);

        // Add the uint8_t to the return value if it isn't part of an '=' character (indicated by 0xff)
        if (b4[1] != 0xff) ret.push_back(b3[0]);
        if (b4[2] != 0xff) ret.push_back(b3[1]);
        if (b4[3] != 0xff) ret.push_back(b3[2]);
    }

    return ret;
}
