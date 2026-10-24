/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// CprHttpClient, the one IHttpClient that reaches the network, against a server of the
// test on the loopback interface: what goes on the wire for each verb, and how the answer (a
// status is an answer, whatever it is; no answer is a transport error) is reported.
// The server uses POSIX sockets: these tests run where they exist.

#include <string>

#include "cpr_http_client.h"
#include "gtest/gtest.h"
#include "http_client.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <sstream>
#include <thread>

namespace
{
    using namespace ddb;

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

    std::string http(int status, const std::string &reason, const std::string &body,
                     const std::string &extraHeaders = "")
    {
        return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
    }

    bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

    TEST(CprClient, GetSendsTheHeadersAndReturnsStatusBodyAndLowercaseHeaders)
    {
        LoopbackServer server({http(200, "OK", "hello", "Retry-After: 3\r\nX-Thing: A\r\n")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/orgs/o/ds/d");
        request.headers = {{"Cookie", "jwtToken=abc"}};

        const auto res = client.get(request);

        EXPECT_EQ(res.status, 200);
        EXPECT_TRUE(res.ok());
        EXPECT_EQ(res.body, "hello");
        EXPECT_FALSE(res.hasTransportError());
        EXPECT_EQ(res.header("Retry-After"), "3");
        EXPECT_EQ(res.header("retry-after"), "3");
        EXPECT_EQ(res.headers.at("x-thing"), "A");
        EXPECT_EQ(res.header("missing"), "");

        ASSERT_EQ(server.requests.size(), 1u);
        EXPECT_TRUE(server.requests[0].starts_with("GET /orgs/o/ds/d HTTP/1.1\r\n")) << server.requests[0];
        EXPECT_TRUE(contains(server.requests[0], "Cookie: jwtToken=abc\r\n")) << server.requests[0];
    }

    // The client does not judge a status: a 404 and a 500 are responses
    TEST(CprClient, AnErrorStatusIsAResponseNotATransportError)
    {
        LoopbackServer server({http(404, "Not Found", R"({"title":"Not Found"})"), http(500, "Internal Server Error", "boom")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url();
        const auto a = client.get(request);
        EXPECT_EQ(a.status, 404);
        EXPECT_EQ(a.body, R"({"title":"Not Found"})");
        EXPECT_FALSE(a.hasTransportError());
        const auto b = client.get(request);
        EXPECT_EQ(b.status, 500);
        EXPECT_FALSE(b.ok());
    }

    TEST(CprClient, PostSendsTheFormEncodedInOrder)
    {
        LoopbackServer server({http(200, "OK", "{}")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/users/authenticate");
        request.form = {{"username", "alice"}, {"password", "p w&d=1"}};

        const auto res = client.post(request);

        EXPECT_EQ(res.status, 200);
        const auto &sent = server.requests.at(0);
        EXPECT_TRUE(sent.starts_with("POST /users/authenticate HTTP/1.1\r\n")) << sent;
        EXPECT_TRUE(contains(sent, "Content-Type: application/x-www-form-urlencoded\r\n")) << sent;
        EXPECT_TRUE(sent.ends_with("\r\n\r\nusername=alice&password=p%20w%26d%3D1")) << sent;
    }

    TEST(CprClient, APostWithoutFieldsHasNoBody)
    {
        LoopbackServer server({http(200, "OK", "{}")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/share/commit/t");
        EXPECT_EQ(client.post(request).status, 200);
        EXPECT_TRUE(server.requests.at(0).ends_with("\r\n\r\n")) << server.requests.at(0);
    }

    TEST(CprClient, MultipartSendsTextPartsAndFilesUnderTheirOwnName)
    {
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("dir/photo one.txt"), "file-bytes");
        LoopbackServer server({http(200, "OK", "{}")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/push/upload");
        request.headers = {{"Authorization", "Bearer t"}};
        request.parts = {HttpPart::fromFile("file", file), HttpPart::text("path", "dir/photo one.txt"),
                         HttpPart::text("token", "tok")};
        request.limits.total = std::chrono::milliseconds(20000);

        EXPECT_EQ(client.multipart(request).status, 200);

        const auto &sent = server.requests.at(0);
        EXPECT_TRUE(contains(sent, "Authorization: Bearer t\r\n")) << sent;
        EXPECT_TRUE(contains(sent, "Content-Type: multipart/form-data; boundary=")) << sent;
        EXPECT_TRUE(contains(sent, R"(name="file"; filename="photo one.txt")")) << sent;
        EXPECT_TRUE(contains(sent, "\r\n\r\nfile-bytes\r\n")) << sent;
        EXPECT_TRUE(contains(sent, "name=\"path\"\r\n\r\ndir/photo one.txt\r\n")) << sent;
        EXPECT_TRUE(contains(sent, "name=\"token\"\r\n\r\ntok\r\n")) << sent;
        // The parts keep their order: file, path, token
        EXPECT_LT(sent.find(R"(name="file")"), sent.find(R"(name="path")"));
        EXPECT_LT(sent.find(R"(name="path")"), sent.find(R"(name="token")"));
    }

    // The share upload had a 10 s cap on the whole transfer, which a large file on a slow link
    // never met. The limit that stays is on a transfer that stopped moving.
    TEST(CprClient, ATransferThatStopsMovingIsAbortedByTheStallLimit)
    {
        using std::chrono::milliseconds;
        TestArea ta(TEST_NAME, true);
        const auto file = writeFile(ta.getPath("big.bin"), "bytes");
        LoopbackServer server({LoopbackServer::Hang});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/share/upload/t");
        request.parts = {HttpPart::fromFile("file", file)};
        request.limits = HttpLimits{milliseconds(0), milliseconds(5000), milliseconds(1000)};

        const auto started = std::chrono::steady_clock::now();
        const auto res = client.multipart(request);
        const auto waited = std::chrono::steady_clock::now() - started;

        EXPECT_EQ(res.status, 0);
        EXPECT_EQ(res.transport, HttpTransportError::Other) << res.transportMessage;
        EXPECT_LT(waited, std::chrono::seconds(20)); // it ended: the server never answers
    }

    TEST(CprClient, DownloadWritesTheBodyToTheSinkAndNotToTheResponse)
    {
        const std::string body(100000, 'x');
        LoopbackServer server({http(200, "OK", body)});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/download?path=a.txt");
        std::ostringstream sink;
        bool progressed = false;
        request.progress = [&](const HttpProgress &p) {
            progressed = progressed || p.downloadNow > 0;
            return true;
        };

        const auto res = client.download(request, sink);

        EXPECT_EQ(res.status, 200);
        EXPECT_TRUE(res.body.empty());
        EXPECT_EQ(sink.str(), body);
        EXPECT_TRUE(progressed);
        EXPECT_TRUE(server.requests.at(0).starts_with("GET /download?path=a.txt HTTP/1.1\r\n"));
    }

    TEST(CprClient, DownloadWithAFormIsAPost)
    {
        LoopbackServer server({http(200, "OK", "zip")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/download");
        request.form = {{"path", "a.txt,b.txt"}};
        std::ostringstream sink;
        EXPECT_EQ(client.download(request, sink).status, 200);
        EXPECT_EQ(sink.str(), "zip");
        EXPECT_TRUE(server.requests.at(0).starts_with("POST /download HTTP/1.1\r\n"));
        EXPECT_TRUE(server.requests.at(0).ends_with("path=a.txt%2Cb.txt")) << server.requests.at(0);
    }

    TEST(CprClient, TheErrorBodyOfADownloadGoesToTheSinkToo)
    {
        LoopbackServer server({http(403, "Forbidden", R"({"title":"Forbidden"})")});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url("/download");
        std::ostringstream sink;
        const auto res = client.download(request, sink);
        EXPECT_EQ(res.status, 403);
        EXPECT_EQ(sink.str(), R"({"title":"Forbidden"})");
    }

    TEST(CprClient, ARefusedConnectionIsATransportErrorWithStatusZero)
    {
        CprHttpClient client;
        HttpRequest request;
        request.url = "http://127.0.0.1:1/";
        const auto res = client.get(request);
        EXPECT_EQ(res.status, 0);
        EXPECT_FALSE(res.ok());
        EXPECT_EQ(res.transport, HttpTransportError::CouldNotConnect);
        EXPECT_FALSE(res.transportMessage.empty());
    }

    TEST(CprClient, AServerThatHangsUpIsAnotherTransportError)
    {
        LoopbackServer server({""});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url();
        const auto res = client.get(request);
        EXPECT_EQ(res.status, 0);
        EXPECT_EQ(res.transport, HttpTransportError::Other);
        EXPECT_FALSE(res.transportMessage.empty());
    }

    TEST(CprClient, ASinkThatFailsEndsTheDownload)
    {
        LoopbackServer server({http(200, "OK", std::string(50000, 'y'))});
        CprHttpClient client;
        HttpRequest request;
        request.url = server.url();
        std::ostringstream sink;
        sink.setstate(std::ios::badbit);
        const auto res = client.download(request, sink);
        EXPECT_TRUE(res.hasTransportError());
    }

    TEST(CprClient, TheDefaultClientIsOneAndIsTheCprOne)
    {
        EXPECT_EQ(&defaultHttpClient(), &defaultHttpClient());
        EXPECT_NE(dynamic_cast<CprHttpClient *>(&defaultHttpClient()), nullptr);
    }

}

#endif // _WIN32

namespace
{
    using namespace ddb;

    TEST(HttpClient, UrlEncodeEscapesWhatAQueryValueCannotHold)
    {
        EXPECT_EQ(urlEncode("plain-file_1.txt~"), "plain-file_1.txt~");
        EXPECT_EQ(urlEncode("a b/c&d=e"), "a%20b%2Fc%26d%3De");
        EXPECT_EQ(urlEncode("caf\xC3\xA9"), "caf%C3%A9");
        EXPECT_EQ(urlEncode(""), "");
    }
}

