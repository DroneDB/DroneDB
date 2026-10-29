/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef LOOPBACK_SERVER_H
#define LOOPBACK_SERVER_H

// An HTTP server of a test on the loopback interface, for the code that reaches the network
// (the HTTP client, the download of a remote raster, what GDAL would fetch). It uses POSIX
// sockets: the tests that include it run where they exist.

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace testutils
{
    // Answers each connection with the next scripted reply (the last repeats), keeps what it
    // received, and closes: HTTP/1.1 with Connection: close. An empty reply closes the
    // connection without answering; Hang keeps it open, silent, until the client leaves.
    class LoopbackServer
    {
    public:
        static constexpr const char *Hang = "<hang>";

    private:
        int listener = -1;
        std::thread worker;
        std::atomic<bool> stopping{false};
        std::vector<std::string> replies;

        static std::string readRequest(int fd)
        {
            std::string data;
            char buf[4096];
            size_t headEnd = std::string::npos;
            size_t want = 0;
            for (;;)
            {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                data.append(buf, static_cast<size_t>(n));
                if (headEnd == std::string::npos && (headEnd = data.find("\r\n\r\n")) != std::string::npos)
                {
                    const auto pos = data.find("Content-Length: ");
                    want = headEnd + 4 + (pos != std::string::npos ? std::stoul(data.substr(pos + 16)) : 0);
                }
                if (headEnd != std::string::npos && data.size() >= want)
                    break;
            }
            return data;
        }

    public:
        int port = 0;
        std::vector<std::string> requests;

        explicit LoopbackServer(std::vector<std::string> scripted) : replies(std::move(scripted))
        {
            listener = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            EXPECT_EQ(::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
            EXPECT_EQ(::listen(listener, 8), 0);
            socklen_t len = sizeof(addr);
            ::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);
            port = ntohs(addr.sin_port);
            worker = std::thread([this] {
                size_t i = 0;
                while (!stopping)
                {
                    const int fd = ::accept(listener, nullptr, nullptr);
                    if (fd < 0)
                        break;
                    requests.push_back(readRequest(fd));
                    const std::string &reply = this->replies[std::min(i++, this->replies.size() - 1)];
                    if (reply == Hang)
                    {
                        char byte;
                        while (!stopping && ::recv(fd, &byte, 1, 0) > 0)
                        {
                        }
                    }
                    else if (!reply.empty())
                        ::send(fd, reply.data(), reply.size(), 0);
                    ::close(fd);
                }
            });
        }

        ~LoopbackServer()
        {
            stopping = true;
            ::shutdown(listener, SHUT_RDWR);
            ::close(listener);
            // wake a blocked accept on platforms where shutdown does not
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<uint16_t>(port));
            ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            ::close(fd);
            worker.join();
        }

        std::string url(const std::string &path = "/") const { return "http://127.0.0.1:" + std::to_string(port) + path; }
    };

    // A whole HTTP/1.1 reply with its Content-Length
    inline std::string http(int status, const std::string &reason, const std::string &body,
                            const std::string &extraHeaders = "")
    {
        return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
    }
} // namespace testutils

#endif // _WIN32

#endif // LOOPBACK_SERVER_H
