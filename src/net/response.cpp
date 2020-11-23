/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "response.h"

#include "exceptions.h"
#include "logger.h"

namespace ddb::net {

Response::Response() : statusCode(0), buf(nullptr), bufSize(0) {}

Response::Response(Response &&other) noexcept
    : statusCode(0), buf(nullptr), bufSize(0) {
    // Copy
    statusCode = other.statusCode;
    buf = other.buf;
    bufSize = other.bufSize;

    // Prevent multiple de-allocations
    other.statusCode = 0;
    other.buf = nullptr;
    other.bufSize = 0;
}

Response &Response::operator=(Response &&other) noexcept {
    if (this != &other) {
        // Free the existing resource.
        free(buf);

        // Copy the data
        buf = other.buf;
        bufSize = other.bufSize;

        // Release the data pointer from the source object so that
        // the destructor does not free the memory multiple times.
        other.buf = nullptr;
        other.bufSize = 0;
    }

    return *this;
}

Response::~Response() {
    if (buf) {
        free(buf);
        buf = nullptr;
    }
}

char *Response::getData() { return buf; }

std::string Response::getText() {
    if (buf != nullptr) {
        return std::string(buf);
    }
    return "";
}

bool Response::hasData() { return buf != nullptr; }

json Response::getJSON() {
    if (getData() == nullptr) return json();

    try {
        return json::parse(getData());
    } catch (const json::parse_error &) {
        throw JSONException("Invalid JSON: " + std::string(getData()));
    }
}

long Response::status() { return statusCode; }

size_t Response::WriteCallback(void *contents, size_t size, size_t nmemb,
                               void *userp) {
    const size_t realsize = size * nmemb;
    auto *res = static_cast<Response *>(userp);

    char *ptr;
    if (!res->buf) {
        res->buf = static_cast<char *>(malloc(realsize + 1));
        ptr = res->buf;
    } else {
        ptr =
            static_cast<char *>(realloc(res->buf, res->bufSize + realsize + 1));
    }
    if (!ptr) {
        /* out of memory! */
        LOGW << "not enough memory (realloc returned NULL)\n";
        return 0;
    }

    res->buf = ptr;
    memcpy(&(res->buf[res->bufSize]), contents, realsize);
    res->bufSize += realsize;
    res->buf[res->bufSize] = 0;

    return realsize;
}

}  // namespace ddb::net
