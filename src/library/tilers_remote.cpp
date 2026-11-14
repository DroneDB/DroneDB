/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The range reads ddb-tilers asks for a remote point cloud, done with libddb's HTTP client.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <ostream>
#include <streambuf>
#include <string>

#include "http_client.h"
#include "tilers_call.h"

namespace ddb::tilers
{
    namespace
    {
        // Receives a body into a buffer of a fixed size: a longer body fails the stream, which ends
        // the transfer (a server that ignores the range sends the whole file)
        class FixedSink : public std::streambuf
        {
            char *data_;
            size_t capacity_;
            size_t size_ = 0;

        protected:
            std::streamsize xsputn(const char *s, std::streamsize n) override
            {
                const size_t take = (std::min)(static_cast<size_t>(n), capacity_ - size_);
                std::memcpy(data_ + size_, s, take);
                size_ += take;
                return static_cast<std::streamsize>(take);
            }
            int_type overflow(int_type c) override
            {
                if (traits_type::eq_int_type(c, traits_type::eof()) || size_ == capacity_)
                    return traits_type::eof();
                data_[size_++] = traits_type::to_char_type(c);
                return c;
            }

        public:
            FixedSink(void *data, size_t capacity) : data_(static_cast<char *>(data)), capacity_(capacity) {}
            size_t size() const { return size_; }
        };

        void say(char *error, size_t errorSize, const std::string &text)
        {
            if (error != nullptr && errorSize > 0)
                std::snprintf(error, errorSize, "%s", text.c_str());
        }

        // GET of bytes [first, first + size) of url into data. DDBT_ERR_NONE when the server sent
        // exactly them (206 Partial Content); the reason in error otherwise.
        ddbtErr getRange(const char *url, uint64_t first, void *data, size_t size, HttpResponse &response,
                         char *error, size_t errorSize)
        {
            HttpRequest request;
            request.url = url;
            request.headers = {{"Range", "bytes=" + std::to_string(first) + "-" + std::to_string(first + size - 1)}};
            request.limits.connect = std::chrono::seconds(30);
            request.limits.stall = std::chrono::seconds(60);
            FixedSink buffer(data, size);
            std::ostream sink(&buffer);
            response = defaultHttpClient().download(request, sink);
            if (response.status == 404 || response.status == 410)
            {
                say(error, errorSize, "HTTP " + std::to_string(response.status));
                return DDBT_ERR_NOTFOUND;
            }
            if (response.status == 200)
            {
                say(error, errorSize, "the server does not answer range requests (HTTP 200)");
                return DDBT_ERR_EXCEPTION;
            }
            if (response.status != 206 || response.hasTransportError())
            {
                say(error, errorSize, response.status == 0 ? response.transportMessage
                                                           : "HTTP " + std::to_string(response.status) +
                                                                 (response.hasTransportError()
                                                                      ? ": " + response.transportMessage
                                                                      : std::string()));
                return DDBT_ERR_EXCEPTION;
            }
            if (buffer.size() != size)
            {
                say(error, errorSize,
                    "the server sent " + std::to_string(buffer.size()) + " bytes for " + std::to_string(size));
                return DDBT_ERR_EXCEPTION;
            }
            return DDBT_ERR_NONE;
        }

        // The size of the object from the Content-Range of a reply to a range request
        // ("bytes 0-0/1234"); 0 when it has none
        uint64_t totalOf(const HttpResponse &response)
        {
            const std::string range = response.header("Content-Range");
            const size_t slash = range.rfind('/');
            if (slash == std::string::npos || slash + 1 >= range.size())
                return 0;
            try
            {
                size_t used = 0;
                const unsigned long long total = std::stoull(range.substr(slash + 1), &used);
                return used == range.size() - slash - 1 ? total : 0;
            }
            catch (const std::exception &)
            {
                return 0;
            }
        }

        ddbtErr stat(const char *url, uint64_t *outSize, char *version, size_t versionSize, char *error,
                     size_t errorSize, void *)
        {
            uint8_t first = 0;
            HttpResponse response;
            const ddbtErr rc = getRange(url, 0, &first, 1, response, error, errorSize);
            if (rc != DDBT_ERR_NONE)
                return rc;
            const uint64_t size = totalOf(response);
            if (size == 0)
            {
                say(error, errorSize, "the server gives no size (Content-Range: " +
                                          response.header("Content-Range") + ")");
                return DDBT_ERR_EXCEPTION;
            }
            *outSize = size;
            std::string tag = response.header("ETag");
            if (tag.empty())
                tag = response.header("Last-Modified");
            std::snprintf(version, versionSize, "%s", tag.c_str());
            return DDBT_ERR_NONE;
        }

        ddbtErr read(const char *url, uint64_t offset, uint8_t *buffer, size_t size, char *error, size_t errorSize,
                     void *)
        {
            HttpResponse response;
            return getRange(url, offset, buffer, size, response, error, errorSize);
        }
    } // namespace

    void useHttpClientForRemoteReads()
    {
        const ddbtRemoteReader reader{sizeof(ddbtRemoteReader), &stat, &read, nullptr};
        check(ddbtSetRemoteReader(&reader));
    }
} // namespace ddb::tilers
