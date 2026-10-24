/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-11: RegistryClient against a fake IHttpClient: the login, the token and its expiry, the
// credentials kept in the user profile, the mapping of error responses and transport errors to
// exceptions, and what is tried again. No network, no sleeping, no user profile.

#include <fstream>
#include <string>

#include "exceptions.h"
#include "fake_http_client.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "mzip.h"
#include "registry_client.h"
#include "test.h"
#include "testarea.h"

namespace
{
    using namespace ddb;
    using fake::jsonReply;
    using fake::problem;
    using fake::reply;
    using fake::transportFailure;

    constexpr const char *Url = "https://registry.test";

    std::string messageOf(const std::function<void()> &fn)
    {
        try
        {
            fn();
        }
        catch (const std::exception &e)
        {
            return e.what();
        }
        return "<no exception>";
    }

    template <typename E>
    void expectThrows(const std::function<void()> &fn, const std::string &message)
    {
        try
        {
            fn();
            FAIL() << "nothing was thrown, expected: " << message;
        }
        catch (const E &e)
        {
            EXPECT_EQ(std::string(e.what()), message);
        }
    }

    struct ClientFixture
    {
        TestArea ta;
        fake::Network net;
        RegistryClient client;

        explicit ClientFixture(const std::string &name, const std::string &url = Url)
            : ta(name, true), net(ta.getPath("auth.json")), client(url, true, net.deps())
        {
        }
    };

    // ---------------------------------------------------------------------------- the URL

    TEST(Dq11RegistryClientUrl, IsNormalized)
    {
        ClientFixture f(TEST_NAME);
        EXPECT_EQ(RegistryClient("registry.test", true, f.net.deps()).getUrl("/x"), "https://registry.test/x");
        EXPECT_EQ(RegistryClient("http://registry.test:8080", true, f.net.deps()).getUrl(), "http://registry.test:8080");
        EXPECT_EQ(RegistryClient("https://registry.test:443", true, f.net.deps()).getUrl(), "https://registry.test");
        EXPECT_EQ(RegistryClient("", true, f.net.deps()).getUrl(), std::string("https://") + DEFAULT_REGISTRY);
    }

    // "ftp://x" has a scheme, so it never was "no scheme": it was sent as https://ftp://x
    TEST(Dq11RegistryClientUrl, OnlyHttpAndHttpsAreAccepted)
    {
        ClientFixture f(TEST_NAME);
        for (const char *bad : {"ftp://registry.test", "file:///etc/passwd", "ssh://registry.test", "ws://registry.test",
                                "javascript://registry.test", "https://", "http:///x", "://registry.test"})
            EXPECT_THROW(RegistryClient(bad, true, f.net.deps()), InvalidArgsException) << bad;

        try
        {
            RegistryClient("ftp://registry.test", true, f.net.deps());
            FAIL();
        }
        catch (const InvalidArgsException &e)
        {
            EXPECT_STREQ(e.what(), "Invalid registry URL 'ftp://registry.test': only http and https are supported");
        }
    }

    TEST(Dq11RegistryClientUrl, TheSchemeIsReadInAnyCaseAndTheBasePathIsKept)
    {
        ClientFixture f(TEST_NAME);
        EXPECT_EQ(RegistryClient("HTTP://Registry.test:8080/api/", true, f.net.deps()).getUrl("/x"),
                  "http://Registry.test:8080/api/x");
        EXPECT_EQ(RegistryClient("  https://registry.test/  ", true, f.net.deps()).getUrl("/x"), "https://registry.test/x");
        // a port is the default of its own scheme only
        EXPECT_EQ(RegistryClient("http://registry.test:443", true, f.net.deps()).getUrl(), "http://registry.test:443");
        EXPECT_EQ(RegistryClient("https://registry.test:80", true, f.net.deps()).getUrl(), "https://registry.test:80");
    }

    // ---------------------------------------------------------------------------- login

    TEST(Dq11RegistryClientLogin, SendsTheFormKeepsTheTokenAndSavesTheCredentials)
    {
        ClientFixture f(TEST_NAME);
        f.net.acceptLogin("tok-1", 3600);

        EXPECT_EQ(f.client.login("alice", "pa:ss word"), "tok-1");

        ASSERT_EQ(f.net.http.calls.size(), 1u);
        const auto &call = f.net.http.calls[0];
        EXPECT_EQ(call.method, "POST");
        EXPECT_EQ(call.path, "https://registry.test/users/authenticate");
        ASSERT_EQ(call.request.form.size(), 2u);
        EXPECT_EQ(call.request.form[0], std::make_pair(std::string("username"), std::string("alice")));
        EXPECT_EQ(call.request.form[1], std::make_pair(std::string("password"), std::string("pa:ss word")));
        EXPECT_TRUE(call.request.headers.empty()); // nothing to present yet
        EXPECT_TRUE(call.request.verifySsl);

        EXPECT_EQ(f.client.getAuthToken(), "tok-1");
        EXPECT_EQ(f.client.getTokenExpiration(), f.net.now + 3600);

        // Saved for next time (under the normalized URL), and read back from the file
        const AuthCredentials saved = AuthManager(f.ta.getPath("auth.json")).loadCredentials(Url);
        EXPECT_EQ(saved.username, "alice");
        EXPECT_EQ(saved.password, "pa:ss word");
    }

    TEST(Dq11RegistryClientLogin, TheSslSettingReachesTheRequest)
    {
        ClientFixture f(TEST_NAME);
        f.net.acceptLogin();
        f.client.setSslVerify(false);
        f.client.login("alice", "x");
        EXPECT_FALSE(f.net.http.calls.at(0).request.verifySsl);
    }

    TEST(Dq11RegistryClientLogin, ProblemDetailsMessageIsReported)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", problem(401, "Unauthorized", "Invalid credentials"));
        expectThrows<AuthException>([&] { f.client.login("alice", "bad"); }, "Login failed: Invalid credentials");
        EXPECT_EQ(f.net.http.calls.size(), 1u); // a refusal is not tried again
        EXPECT_TRUE(f.net.sleeps.empty());
        EXPECT_TRUE(f.client.getAuthToken().empty());
        // And nothing is saved for a login that failed
        EXPECT_TRUE(AuthManager(f.ta.getPath("auth.json")).loadCredentials(Url).empty());
    }

    TEST(Dq11RegistryClientLogin, LegacyBodyAndBareStatus)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", reply(401, R"({"error":"Unauthorized"})"));
        expectThrows<AuthException>([&] { f.client.login("alice", "bad"); }, "Login failed: Unauthorized");

        f.net.http.on("POST", "/users/authenticate", reply(400, "<html>Bad Request</html>"));
        // (the first route still has its reply: a route answers while it has replies)
        ClientFixture g(std::string(TEST_NAME) + "2");
        g.net.http.on("POST", "/users/authenticate", reply(400, "<html>Bad Request</html>"));
        expectThrows<AuthException>([&] { g.client.login("alice", "bad"); }, "Login failed: host returned 400");
    }

    TEST(Dq11RegistryClientLogin, AMalformedAnswerIsAnAuthError)
    {
        for (const std::string body : {"not json", "{}", R"({"token":"t"})", R"({"token":5,"expires":1})"})
        {
            ClientFixture f(std::string(TEST_NAME) + std::to_string(body.size()));
            f.net.http.on("POST", "/users/authenticate", reply(200, body));
            expectThrows<AuthException>([&] { f.client.login("alice", "x"); },
                                        "Login failed: invalid response from the registry");
            EXPECT_TRUE(f.client.getAuthToken().empty()) << body;
        }
    }

    TEST(Dq11RegistryClientLogin, ATransportErrorNamesTheServerAndIsTriedAgain)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", transportFailure(HttpTransportError::CouldNotConnect, "Connection refused"));
        expectThrows<RegistryException>([&] { f.client.login("alice", "x"); },
                                    "Login failed: could not connect to https://registry.test: Connection refused");
        EXPECT_EQ(f.net.http.calls.size(), 3u);   // the network policy: three attempts
        EXPECT_EQ(f.net.sleeps.size(), 2u);       // two waits, recorded and not made
    }

    TEST(Dq11RegistryClientLogin, ATimeoutIsAnotherTransportError)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", transportFailure(HttpTransportError::Other, "Operation timed out"));
        expectThrows<RegistryException>([&] { f.client.login("alice", "x"); },
                                    "Login failed: request to https://registry.test failed: Operation timed out");
    }

    TEST(Dq11RegistryClientLogin, ASevereErrorIsTriedAgainAndSucceeds)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate",
                      {problem(503, "Service Unavailable"), jsonReply({{"token", "tok-2"}, {"expires", f.net.now + 60}})});
        EXPECT_EQ(f.client.login("alice", "x"), "tok-2");
        EXPECT_EQ(f.net.http.calls.size(), 2u);
        EXPECT_EQ(f.net.sleeps.size(), 1u);
    }

    TEST(Dq11RegistryClientLogin, ServerErrorsAreTriedAgainThenReported)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", reply(500, "boom"));
        expectThrows<AuthException>([&] { f.client.login("alice", "x"); }, "Login failed: host returned 500");
        EXPECT_EQ(f.net.http.calls.size(), 3u);
    }

    TEST(Dq11RegistryClientLogin, TooManyRequestsWaitsForRetryAfter)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate",
                      {reply(429, "", {{"retry-after", "7"}}), jsonReply({{"token", "tok-3"}, {"expires", f.net.now + 60}})});
        EXPECT_EQ(f.client.login("alice", "x"), "tok-3");
        ASSERT_EQ(f.net.sleeps.size(), 1u);
        EXPECT_EQ(f.net.sleeps[0], std::chrono::seconds(7));
    }

    TEST(Dq11RegistryClientLogin, ARefusalMarkedNoRetryIsNotTriedAgain)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", problem(503, "Maintenance", "Down for maintenance", true));
        expectThrows<AuthException>([&] { f.client.login("alice", "x"); }, "Login failed: Down for maintenance");
        EXPECT_EQ(f.net.http.calls.size(), 1u);
    }

    TEST(Dq11RegistryClientLogin, WithoutSavedCredentialsThereIsNothingToLogInWith)
    {
        ClientFixture f(TEST_NAME);
        expectThrows<InvalidArgsException>([&] { f.client.login(); },
                                           "No stored credentials for registry at 'https://registry.test'");
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    TEST(Dq11RegistryClientLogin, UsesTheSavedCredentials)
    {
        ClientFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("bob", "secret"));
        f.net.acceptLogin("tok-b");
        EXPECT_EQ(f.client.login(), "tok-b");
        EXPECT_EQ(f.net.http.calls.at(0).field("username"), "bob");
        EXPECT_EQ(f.net.http.calls.at(0).field("password"), "secret");
    }

    TEST(Dq11RegistryClientLogin, LogoutDeletesTheCredentials)
    {
        ClientFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("bob", "secret"));
        EXPECT_TRUE(f.client.logout());
        EXPECT_FALSE(f.client.logout());
        EXPECT_TRUE(AuthManager(f.ta.getPath("auth.json")).loadCredentials(Url).empty());
    }

    // ---------------------------------------------------------------------------- token expiry

    TEST(Dq11RegistryClientToken, NoTokenNoRefresh)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/x", reply(200, "ok"));
        f.net.now += 100000;
        f.client.ensureTokenValidity();
        EXPECT_EQ(f.client.get("/x", AuthScheme::Cookie).status, 200); // anonymous reads work
        EXPECT_EQ(f.net.http.calls.size(), 1u);
    }

    TEST(Dq11RegistryClientToken, AValidTokenIsNotRefreshed)
    {
        ClientFixture f(TEST_NAME);
        f.net.acceptLogin("tok-1", 3600);
        f.client.login("alice", "x");
        f.net.http.on("GET", "/x", reply(200, "ok"));

        f.net.now += 3599;
        f.client.get("/x", AuthScheme::Cookie);
        EXPECT_EQ(f.net.http.callsTo("POST", "/users/authenticate").size(), 1u);
    }

    TEST(Dq11RegistryClientToken, AnExpiredTokenIsRefreshedWithTheSavedCredentialsBeforeTheRequest)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate",
                      {jsonReply({{"token", "tok-1"}, {"expires", f.net.now + 3600}}),
                       jsonReply({{"token", "tok-2"}, {"expires", f.net.now + 7200}})});
        f.client.login("alice", "pw");
        f.net.http.on("GET", "/x", reply(200, "ok"));

        f.net.now += 3600; // the expiry itself counts as expired
        EXPECT_EQ(f.client.get("/x", AuthScheme::Cookie).status, 200);

        ASSERT_EQ(f.net.http.calls.size(), 3u);
        EXPECT_EQ(f.net.http.calls[1].path, "https://registry.test/users/authenticate");
        EXPECT_EQ(f.net.http.calls[1].field("username"), "alice");
        EXPECT_EQ(f.net.http.calls[1].field("password"), "pw");
        // The request that follows carries the new token
        EXPECT_EQ(f.net.http.calls[2].header("Cookie"), "jwtToken=tok-2");
        EXPECT_EQ(f.client.getTokenExpiration(), f.net.now - 3600 + 7200);
    }

    TEST(Dq11RegistryClientToken, ARefreshTheServerRefusesIsAnAuthError)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate",
                      {jsonReply({{"token", "tok-1"}, {"expires", f.net.now + 10}}),
                       problem(401, "Unauthorized", "Password changed")});
        f.client.login("alice", "pw");
        f.net.now += 11;
        expectThrows<AuthException>([&] { f.client.get("/x", AuthScheme::Cookie); }, "Login failed: Password changed");
    }

    TEST(Dq11RegistryClientToken, ExpiredWithoutSavedCredentialsCannotBeRefreshed)
    {
        ClientFixture f(TEST_NAME);
        f.net.acceptLogin("tok-1", 10);
        f.client.login("alice", "pw");
        f.net.credentials.deleteCredentials(Url);
        f.net.now += 11;
        EXPECT_THROW(f.client.get("/x", AuthScheme::Cookie), InvalidArgsException);
    }

    // ---------------------------------------------------------------------------- requests

    TEST(Dq11RegistryClientRequests, EachEndpointKeepsItsWayOfPresentingTheToken)
    {
        ClientFixture f(TEST_NAME);
        f.net.acceptLogin("tok-1");
        f.client.login("alice", "pw");
        f.net.http.on("GET", "/a", reply(200));
        f.net.http.on("POST", "/b", reply(200));
        f.net.http.on("MULTIPART", "/c", reply(200));
        f.net.http.on("DOWNLOAD", "/d", reply(200, "bytes"));

        f.client.get("/a", AuthScheme::Cookie);
        f.client.postForm("/b", {{"k", "v"}}, AuthScheme::Bearer);
        f.client.postMultipart("/c", {HttpPart::text("path", "p")}, AuthScheme::Bearer);
        std::ostringstream sink;
        f.client.download("/d", AuthScheme::Cookie, sink);

        EXPECT_EQ(f.net.http.callsTo("GET", "/a").at(0).header("Cookie"), "jwtToken=tok-1");
        EXPECT_EQ(f.net.http.callsTo("GET", "/a").at(0).header("Authorization"), "");
        EXPECT_EQ(f.net.http.callsTo("POST", "/b").at(0).header("Authorization"), "Bearer tok-1");
        EXPECT_EQ(f.net.http.callsTo("POST", "/b").at(0).header("Cookie"), "");
        EXPECT_EQ(f.net.http.callsTo("MULTIPART", "/c").at(0).header("Authorization"), "Bearer tok-1");
        EXPECT_EQ(f.net.http.callsTo("DOWNLOAD", "/d").at(0).header("Cookie"), "jwtToken=tok-1");
        EXPECT_EQ(sink.str(), "bytes");
        EXPECT_EQ(f.net.http.callsTo("GET", "/a").at(0).path, "https://registry.test/a");
    }

    TEST(Dq11RegistryClientRequests, WithoutALoginTheCookieIsEmptyAsItAlwaysWas)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/a", reply(200));
        f.client.get("/a", AuthScheme::Cookie);
        EXPECT_EQ(f.net.http.calls.at(0).header("Cookie"), "jwtToken=");
    }

    TEST(Dq11RegistryClientRequests, GetsAreTriedAgainOnTransportErrorsServerErrorsAndTooManyRequests)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/a", {transportFailure(), problem(503, "Busy"), reply(502, "bad gateway"), reply(200, "ok")});
        // Three attempts: the transport error, the 503, the 502: the last response is returned
        auto res = f.client.get("/a", AuthScheme::Cookie);
        EXPECT_EQ(res.status, 502);
        EXPECT_EQ(f.net.http.calls.size(), 3u);

        ClientFixture g(std::string(TEST_NAME) + "b");
        g.net.http.on("GET", "/a", {reply(429, "", {{"retry-after", "2"}}), reply(200, "ok")});
        EXPECT_EQ(g.client.get("/a", AuthScheme::Cookie).status, 200);
        EXPECT_EQ(g.net.sleeps.at(0), std::chrono::seconds(2));
    }

    TEST(Dq11RegistryClientRequests, WhatTheServerRefusedIsNotTriedAgain)
    {
        for (long status : {400, 401, 403, 404, 409, 413})
        {
            ClientFixture f(std::string(TEST_NAME) + std::to_string(status));
            f.net.http.on("GET", "/a", reply(status, "no"));
            EXPECT_EQ(f.client.get("/a", AuthScheme::Cookie).status, status);
            EXPECT_EQ(f.net.http.calls.size(), 1u) << status;
        }
    }

    TEST(Dq11RegistryClientRequests, ANoRetryErrorIsNotTriedAgainEvenWhenItIsA5xx)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/a", problem(507, "Insufficient Storage", "Quota exceeded", true));
        EXPECT_EQ(f.client.get("/a", AuthScheme::Cookie).status, 507);
        EXPECT_EQ(f.net.http.calls.size(), 1u);
    }

    // What may have reached the server is sent once: the client never repeats these behind the
    // caller's back
    TEST(Dq11RegistryClientRequests, UploadsFormsAndDownloadsAreNotTriedAgain)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/form", problem(503, "Busy"));
        f.net.http.on("MULTIPART", "/up", problem(503, "Busy"));
        f.net.http.on("DOWNLOAD", "/down", problem(503, "Busy"));
        f.net.http.on("DOWNLOAD-POST", "/downpost", problem(503, "Busy"));

        EXPECT_EQ(f.client.postForm("/form", {{"k", "v"}}, AuthScheme::Bearer).status, 503);
        EXPECT_EQ(f.client.postMultipart("/up", {}, AuthScheme::Bearer).status, 503);
        std::ostringstream sink;
        EXPECT_EQ(f.client.download("/down", AuthScheme::Cookie, sink).status, 503);
        EXPECT_EQ(f.client.download("/downpost", AuthScheme::Cookie, sink, nullptr, {{"path", "a"}}).status, 503);

        EXPECT_EQ(f.net.http.calls.size(), 4u);
        EXPECT_TRUE(f.net.sleeps.empty());
    }

    TEST(Dq11RegistryClientRequests, AReadDoneWithAPostMayBeTriedAgain)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/read", {problem(503, "Busy"), reply(200, "[]")});
        EXPECT_EQ(f.client.postForm("/read", {{"ids", "[]"}}, AuthScheme::Cookie, true).status, 200);
        EXPECT_EQ(f.net.http.calls.size(), 2u);
    }

    TEST(Dq11RegistryClientRequests, TheMultipartLimitsAndProgressReachTheRequest)
    {
        using std::chrono::milliseconds;
        ClientFixture f(TEST_NAME);
        f.net.http.on("MULTIPART", "/up", reply(200));
        f.net.http.progressEvents = {HttpProgress{0, 0, 10, 5}};
        size_t uploaded = 0;
        f.client.postMultipart("/up", {}, AuthScheme::Bearer, HttpLimits{milliseconds(0), milliseconds(5000), milliseconds(30000)},
                               [&](const HttpProgress &p) { uploaded = p.uploadNow; return true; });
        const auto &limits = f.net.http.calls.at(0).request.limits;
        EXPECT_EQ(limits.total, milliseconds(0));
        EXPECT_EQ(limits.connect, milliseconds(5000));
        EXPECT_EQ(limits.stall, milliseconds(30000));
        EXPECT_EQ(uploaded, 5u);

        // An upload given no limits has none
        f.client.postMultipart("/up", {}, AuthScheme::Bearer);
        EXPECT_EQ(f.net.http.calls.at(1).request.limits.total, milliseconds(0));
        EXPECT_EQ(f.net.http.calls.at(1).request.limits.connect, milliseconds(0));
        EXPECT_EQ(f.net.http.calls.at(1).request.limits.stall, milliseconds(0));
    }

    // ---------------------------------------------------------------------------- errors

    TEST(Dq11RegistryClientErrors, A401IsAnAuthErrorAndA403IsNot)
    {
        ClientFixture f(TEST_NAME);
        expectThrows<AuthException>([&] { f.client.expectOk(problem(401, "Unauthorized", "Token expired")); },
                                    "Unauthorized");

        // The credentials were accepted, the user may not do this: asking for others would not help
        const std::string message = messageOf([&] { f.client.expectOk(problem(403, "Forbidden", "No write access")); });
        EXPECT_EQ(message, "Error response from registry: No write access");
        EXPECT_THROW(f.client.expectOk(problem(403, "Forbidden")), RegistryException);
        try
        {
            f.client.expectOk(problem(403, "Forbidden"));
        }
        catch (const AuthException &)
        {
            FAIL() << "a 403 must not be an AuthException";
        }
        catch (const RegistryException &)
        {
        }
    }

    TEST(Dq11RegistryClientErrors, TheMessageOfTheBodyOrTheBodyItself)
    {
        ClientFixture f(TEST_NAME);
        EXPECT_NO_THROW(f.client.expectOk(reply(200)));
        expectThrows<RegistryException>([&] { f.client.expectOk(problem(409, "Conflict", "Destination exists")); },
                                        "Error response from registry: Destination exists");
        expectThrows<RegistryException>([&] { f.client.expectOk(reply(400, R"({"error":"Bad tag"})")); },
                                        "Error response from registry: Bad tag");
        expectThrows<RegistryException>([&] { f.client.expectOk(reply(502, "<html>502 Bad Gateway</html>")); },
                                        "Invalid response from registry: <html>502 Bad Gateway</html>");
    }

    TEST(Dq11RegistryClientErrors, ATransportErrorNamesTheServer)
    {
        ClientFixture f(TEST_NAME);
        expectThrows<RegistryException>(
            [&] { f.client.expectOk(transportFailure(HttpTransportError::CouldNotConnect, "Connection refused")); },
            "Registry request failed: could not connect to https://registry.test: Connection refused");
        expectThrows<RegistryException>(
            [&] { f.client.expectOk(transportFailure(HttpTransportError::Other, "SSL certificate problem")); },
            "Registry request failed: request to https://registry.test failed: SSL certificate problem");
    }

    TEST(Dq11RegistryClientErrors, AnOkResponseTheCallerFoundMalformedIsReportedWithItsStatus)
    {
        ClientFixture f(TEST_NAME);
        expectThrows<RegistryException>([&] { f.client.fail(reply(200, "{}")); },
                                        "Invalid response from registry. Returned status: 200");
    }

    // ---------------------------------------------------------------------------- dataset reads

    TEST(Dq11RegistryClientDatasets, DatasetInfo)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/orgs/o/ds/d",
                      jsonReply(nlohmann::json::array({{{"path", ""}, {"hash", ""}, {"type", 1}, {"size", 0}, {"depth", 0}, {"mtime", 5}}})));
        const Entry e = f.client.getDatasetInfo("o", "d");
        EXPECT_EQ(static_cast<int>(e.type), 1);
        EXPECT_EQ(f.net.http.calls.at(0).header("Cookie"), "jwtToken=");
    }

    TEST(Dq11RegistryClientDatasets, DatasetInfoOfAMissingDatasetIsNotFound)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/orgs/o/ds/d", problem(404, "Not Found"));
        expectThrows<RegistryNotFoundException>([&] { f.client.getDatasetInfo("o", "d"); }, "Dataset not found");
    }

    TEST(Dq11RegistryClientDatasets, DatasetInfoEmptyAnswer)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/orgs/o/ds/d", jsonReply(nlohmann::json::array()));
        expectThrows<RegistryException>([&] { f.client.getDatasetInfo("o", "d"); },
                                        "Invalid empty response from registry");
    }

    TEST(Dq11RegistryClientDatasets, StampMustHaveAChecksum)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("GET", "/orgs/o/ds/d/stamp", jsonReply({{"checksum", "abc"}, {"entries", nlohmann::json::array()}}));
        EXPECT_EQ(f.client.getStamp("o", "d")["checksum"], "abc");

        ClientFixture g(std::string(TEST_NAME) + "b");
        g.net.http.on("GET", "/orgs/o/ds/d/stamp", reply(200, R"({"entries":[]})"));
        expectThrows<RegistryException>([&] { g.client.getStamp("o", "d"); }, R"(Invalid stamp: {"entries":[]})");

        ClientFixture h(std::string(TEST_NAME) + "c");
        h.net.http.on("GET", "/orgs/o/ds/d/stamp", problem(401, "Unauthorized"));
        EXPECT_THROW(h.client.getStamp("o", "d"), AuthException);
    }

    TEST(Dq11RegistryClientDatasets, MetaDumpIsAReadDoneWithAPost)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("POST", "/orgs/o/ds/d/meta/dump", {problem(503, "Busy"), jsonReply(nlohmann::json::array({{{"id", "1"}}}))});
        const auto dump = f.client.getMetaDump("o", "d", {"1", "2"});
        EXPECT_EQ(dump.size(), 1u);
        EXPECT_EQ(f.net.http.calls.size(), 2u);
        EXPECT_EQ(f.net.http.calls.at(0).field("ids"), R"(["1","2"])");

        ClientFixture g(std::string(TEST_NAME) + "b");
        g.net.http.on("POST", "/orgs/o/ds/d/meta/dump", reply(200, R"({"a":1})"));
        expectThrows<RegistryException>([&] { g.client.getMetaDump("o", "d", {"1"}); },
                                        R"(Invalid meta dump: {"a":1})");
    }

    TEST(Dq11RegistryClientDatasets, OneFileIsDownloadedWithAGetToItsPlace)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("DOWNLOAD", "/orgs/o/ds/d/download?path=sub%2Fa.txt", reply(200, "alpha"));
        std::ostringstream out;
        f.client.downloadFiles("o", "d", {"sub/a.txt"}, io::toUtf8(f.ta.getFolder("dl")), out);

        std::ifstream in(f.ta.getPath("dl/sub/a.txt"), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        EXPECT_EQ(ss.str(), "alpha");
        EXPECT_EQ(f.net.http.calls.size(), 1u);
    }

    // The Registry reads ?path= with the rules of ASP.NET: an unescaped '&' ended the value, '#'
    // started the fragment, '%' began an escape and a space is not allowed in a URL
    TEST(Dq11RegistryClientDatasets, TheNameOfOneFileIsEncodedInTheQuery)
    {
        struct Case { std::string name, encoded; };
        const std::vector<Case> cases = {
            {"a&b.txt", "a%26b.txt"},
            {"a#b.txt", "a%23b.txt"},
            {"a b.txt", "a%20b.txt"},
            {"100%.txt", "100%25.txt"},
            {"a+b.txt", "a%2Bb.txt"},
            {"a=b;c.txt", "a%3Db%3Bc.txt"},
            {"sub dir/\xC3\xA8.txt", "sub%20dir%2F%C3%A8.txt"}, // UTF-8 bytes
        };
        for (const auto &c : cases)
        {
            ClientFixture f(std::string(TEST_NAME) + "-" + std::to_string(&c - cases.data()));
            f.net.http.on("DOWNLOAD", "/orgs/o/ds/d/download?path=" + c.encoded, reply(200, "x"));
            std::ostringstream out;
            f.client.downloadFiles("o", "d", {c.name}, io::toUtf8(f.ta.getFolder("dl")), out);

            ASSERT_EQ(f.net.http.calls.size(), 1u) << c.name;
            EXPECT_EQ(f.net.http.calls.at(0).path, std::string(f.client.getUrl("/orgs/o/ds/d/download?path=")) + c.encoded);
            EXPECT_TRUE(f.net.http.unexpected.empty()) << c.name;
            EXPECT_TRUE(fs::exists(io::fromUtf8(io::toUtf8(f.ta.getFolder("dl")) + "/" + c.name))) << c.name;
        }
    }

    TEST(Dq11RegistryClientDatasets, SeveralFilesAreAZipOfAPostAndNoTempFileStays)
    {
        ClientFixture f(TEST_NAME);
        // The archive the server would send
        const auto src = f.ta.getFolder("src");
        { std::ofstream(src / "a.txt") << "alpha"; std::ofstream(src / "b.txt") << "beta"; }
        const auto zipFile = f.ta.getPath("src.zip");
        ddb::zip::zipFolder(io::toUtf8(src), io::toUtf8(zipFile), {});
        std::ifstream zin(zipFile, std::ios::binary);
        std::stringstream zbytes;
        zbytes << zin.rdbuf();

        f.net.http.on("DOWNLOAD-POST", "/orgs/o/ds/d/download", reply(200, zbytes.str()));
        const auto dl = f.ta.getFolder("dl");
        std::ostringstream out;
        f.client.downloadFiles("o", "d", {"a.txt", "b.txt"}, io::toUtf8(dl), out);

        EXPECT_EQ(f.net.http.calls.at(0).field("path"), "a.txt,b.txt");
        EXPECT_TRUE(fs::exists(dl / "a.txt"));
        EXPECT_TRUE(fs::exists(dl / "b.txt"));
        size_t entries = 0;
        for (const auto &e : fs::directory_iterator(dl))
        {
            (void)e;
            entries++;
        }
        EXPECT_EQ(entries, 2u); // no .tmp left
    }

    TEST(Dq11RegistryClientDatasets, AFailedArchiveDownloadLeavesNoTempFile)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("DOWNLOAD-POST", "/orgs/o/ds/d/download", problem(403, "Forbidden", "No access"));
        const auto dl = f.ta.getFolder("dl");
        std::ostringstream out;
        expectThrows<RegistryException>([&] { f.client.downloadFiles("o", "d", {"a", "b"}, io::toUtf8(dl), out); },
                                        "Error response from registry: No access");
        EXPECT_TRUE(fs::is_empty(dl));
    }

    // The body of the error went to the file the download was writing: the message is read from it
    TEST(Dq11RegistryClientDatasets, TheErrorOfAFileDownloadKeepsItsMessageAndLeavesNoFile)
    {
        ClientFixture f(TEST_NAME);
        f.net.http.on("DOWNLOAD", "/orgs/o/ds/d/download?path=a.txt", problem(404, "Not Found", "No such file"));
        const auto dl = f.ta.getFolder("dl");
        std::ostringstream out;
        expectThrows<RegistryException>([&] { f.client.downloadFiles("o", "d", {"a.txt"}, io::toUtf8(dl), out); },
                                        "Error response from registry: No such file");
        EXPECT_FALSE(fs::exists(dl / "a.txt"));
    }

    TEST(Dq11RegistryClientDatasets, NoFilesNoRequest)
    {
        ClientFixture f(TEST_NAME);
        std::ostringstream out;
        f.client.downloadFiles("o", "d", {}, io::toUtf8(f.ta.getFolder("dl")), out);
        EXPECT_TRUE(f.net.http.calls.empty());
    }
}
