/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <string>

#include "gtest/gtest.h"
#include "authmanager.h"
#include "registry_client.h"
#include "test.h"
#include "testarea.h"
#include "registryutils.h"
#include "constants.h"
#include "exceptions.h"

namespace
{

    using namespace ddb;

    TEST(registryErrorMessage, ProblemDetailsAndLegacyBodies)
    {
        // RFC 9457 ProblemDetails (Registry RQ-05): detail first, then title
        EXPECT_EQ(RegistryUtils::errorMessage(
                      R"({"type":"t","title":"Conflict","status":409,"detail":"Destination exists","noRetry":true})"),
                  "Destination exists");
        EXPECT_EQ(RegistryUtils::errorMessage(R"({"title":"Forbidden","status":403})"), "Forbidden");
        // Legacy body
        EXPECT_EQ(RegistryUtils::errorMessage(R"({"error":"Invalid credentials","noRetry":true})"),
                  "Invalid credentials");
        // No message: not JSON, not an object, no known field
        EXPECT_EQ(RegistryUtils::errorMessage("<html>502 Bad Gateway</html>"), "");
        EXPECT_EQ(RegistryUtils::errorMessage("[1,2]"), "");
        EXPECT_EQ(RegistryUtils::errorMessage(R"({"status":500})"), "");
    }

    // A request that got no HTTP response has status 0 and a transport error: the CLI said
    // "Login failed: host returned 0"
    HttpResponse transportFailure(HttpTransportError kind, const std::string &message)
    {
        HttpResponse res;
        res.status = 0;
        res.transport = kind;
        res.transportMessage = message;
        return res;
    }

    TEST(registryTransportError, NamesTheUrlAndTheReason)
    {
        EXPECT_EQ(RegistryUtils::transportError(
                      transportFailure(HttpTransportError::CouldNotConnect, "Failed to connect to host port 1"),
                      "http://host:1"),
                  "could not connect to http://host:1: Failed to connect to host port 1");

        // Not a connection problem: the server may be there
        EXPECT_EQ(RegistryUtils::transportError(
                      transportFailure(HttpTransportError::Other, "Operation timed out"), "https://slow"),
                  "request to https://slow failed: Operation timed out");

        EXPECT_EQ(RegistryUtils::transportError(transportFailure(HttpTransportError::Other, ""), "https://tls"),
                  "request to https://tls failed: unknown error");
    }

    TEST(registryTransportError, NoneWhenTheServerAnswered)
    {
        HttpResponse res;
        res.status = 500;
        EXPECT_EQ(RegistryUtils::transportError(res, "http://host"), "");
        res.status = 200;
        EXPECT_EQ(RegistryUtils::transportError(res, "http://host"), "");
    }

    TEST(registryNoRetry, ReadsTheFlagOfAnErrorBody)
    {
        EXPECT_TRUE(RegistryUtils::noRetry(R"({"title":"Quota","status":507,"noRetry":true})"));
        EXPECT_FALSE(RegistryUtils::noRetry(R"({"title":"Busy","status":503,"noRetry":false})"));
        EXPECT_FALSE(RegistryUtils::noRetry(R"({"title":"Busy","status":503})"));
        EXPECT_FALSE(RegistryUtils::noRetry(R"({"noRetry":"true"})"));
        EXPECT_FALSE(RegistryUtils::noRetry("<html>502</html>"));
        EXPECT_FALSE(RegistryUtils::noRetry(""));
    }

    // Nothing listens on port 1 of the loopback interface: the connection is refused at once
    constexpr const char *UnreachableRegistry = "http://127.0.0.1:1";

    // The real client (cpr), with nothing faked: the connection error reaches the user's message.
    // The retries of the network policy run (three attempts) but do not wait here: the sleeper
    // is replaced.
    RegistryDeps noWaitDeps()
    {
        RegistryDeps deps;
        deps.sleep = [](std::chrono::milliseconds) {};
        return deps;
    }

    TEST(registryTransportError, LoginToAnUnreachableServer)
    {
        TestArea ta(TEST_NAME, true);
        AuthManager store(ta.getPath("auth.json"));
        auto deps = noWaitDeps();
        deps.credentials = &store;
        RegistryClient registry(UnreachableRegistry, true, deps);
        try
        {
            registry.login("user", "password");
            FAIL() << "the login of an unreachable server succeeded";
        }
        catch (const AuthException &e)
        {
            const std::string message = e.what();
            EXPECT_NE(message.find("Login failed: could not connect to http://127.0.0.1:1: "), std::string::npos)
                << message;
            EXPECT_EQ(message.find("host returned 0"), std::string::npos) << message;
        }
    }

    // The requests that follow a login report it the same way
    TEST(registryTransportError, ARequestToAnUnreachableServer)
    {
        TestArea ta(TEST_NAME, true);
        AuthManager store(ta.getPath("auth.json"));
        auto deps = noWaitDeps();
        deps.credentials = &store;
        RegistryClient registry(UnreachableRegistry, true, deps);
        const auto res = registry.get("/users/authenticate", AuthScheme::Cookie);
        ASSERT_EQ(res.status, 0);
        ASSERT_EQ(res.transport, HttpTransportError::CouldNotConnect);
        try
        {
            registry.expectOk(res);
            FAIL() << "expectOk accepted a request that got no response";
        }
        catch (const RegistryException &e)
        {
            const std::string message = e.what();
            EXPECT_NE(message.find("could not connect to http://127.0.0.1:1: "), std::string::npos) << message;
            EXPECT_EQ(message.find("Invalid response"), std::string::npos) << message;
        }
    }

    TEST(parseTag, Normal)
    {
        auto s = RegistryUtils::parseTag("test:3000/myorg/myds");
        EXPECT_STREQ(s.registryUrl.c_str(), "https://test:3000");
        EXPECT_STREQ(s.organization.c_str(), "myorg");
        EXPECT_STREQ(s.dataset.c_str(), "myds");

        s = RegistryUtils::parseTag("test/myorg/myds", true);
        EXPECT_STREQ(s.registryUrl.c_str(), "http://test");

        s = RegistryUtils::parseTag("myorg/myds");
        EXPECT_STREQ(s.registryUrl.c_str(), "https://" DEFAULT_REGISTRY);
        EXPECT_STREQ(s.organization.c_str(), "myorg");
        EXPECT_STREQ(s.dataset.c_str(), "myds");

        EXPECT_THROW(
            RegistryUtils::parseTag("myorg"),
            InvalidArgsException);
    }

    TEST(parseTag, ValidNames)
    {
        // Valid: starts with lowercase letter
        auto s = RegistryUtils::parseTag("abc/def");
        EXPECT_STREQ(s.organization.c_str(), "abc");
        EXPECT_STREQ(s.dataset.c_str(), "def");

        // Valid: starts with digit
        s = RegistryUtils::parseTag("0rg/9dataset");
        EXPECT_STREQ(s.organization.c_str(), "0rg");
        EXPECT_STREQ(s.dataset.c_str(), "9dataset");

        // Valid: contains underscores
        s = RegistryUtils::parseTag("my_org/my_dataset");
        EXPECT_STREQ(s.organization.c_str(), "my_org");
        EXPECT_STREQ(s.dataset.c_str(), "my_dataset");

        // Valid: contains dashes
        s = RegistryUtils::parseTag("my-org/my-dataset");
        EXPECT_STREQ(s.organization.c_str(), "my-org");
        EXPECT_STREQ(s.dataset.c_str(), "my-dataset");

        // Valid: mixed valid characters
        s = RegistryUtils::parseTag("org_123-test/ds_456-data");
        EXPECT_STREQ(s.organization.c_str(), "org_123-test");
        EXPECT_STREQ(s.dataset.c_str(), "ds_456-data");

        // Valid: minimum length (2 characters)
        s = RegistryUtils::parseTag("ab/cd");
        EXPECT_STREQ(s.organization.c_str(), "ab");
        EXPECT_STREQ(s.dataset.c_str(), "cd");

        // Valid: maximum length (129 characters)
        std::string maxOrg(129, 'a');
        std::string maxDs(129, 'b');
        s = RegistryUtils::parseTag(maxOrg + "/" + maxDs);
        EXPECT_EQ(s.organization.length(), 129);
        EXPECT_EQ(s.dataset.length(), 129);
    }

    TEST(parseTag, InvalidStartCharacter)
    {
        // Invalid: starts with underscore
        EXPECT_THROW(
            RegistryUtils::parseTag("_org/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/_dataset"),
            InvalidArgsException);

        // Invalid: starts with dash
        EXPECT_THROW(
            RegistryUtils::parseTag("-org/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/-dataset"),
            InvalidArgsException);

        // Invalid: starts with uppercase letter (gets converted to lowercase, but would fail if uppercase was in original)
        // Note: parseTag converts to lowercase, so "Org" becomes "org" which is valid
        // The JavaScript validation would catch this at input time
    }

    TEST(parseTag, InvalidCharacters)
    {
        // Invalid: contains uppercase (will be converted to lowercase by parseTag)
        // But if original validation should reject uppercase, this needs to be tested differently

        // Invalid: contains period
        EXPECT_THROW(
            RegistryUtils::parseTag("org.name/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/data.set"),
            InvalidArgsException);

        // Invalid: contains spaces (will be trimmed, but space in middle is invalid)
        EXPECT_THROW(
            RegistryUtils::parseTag("my org/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/my dataset"),
            InvalidArgsException);

        // Invalid: contains special characters
        EXPECT_THROW(
            RegistryUtils::parseTag("org@name/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/dataset#123"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org$/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/dataset%"),
            InvalidArgsException);
    }

    TEST(parseTag, InvalidLength)
    {
        // Invalid: too short (1 character)
        EXPECT_THROW(
            RegistryUtils::parseTag("a/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/b"),
            InvalidArgsException);

        // Invalid: too long (130 characters)
        std::string tooLongOrg(130, 'a');
        std::string tooLongDs(130, 'b');

        EXPECT_THROW(
            RegistryUtils::parseTag(tooLongOrg + "/dataset"),
            InvalidArgsException);

        EXPECT_THROW(
            RegistryUtils::parseTag("org/" + tooLongDs),
            InvalidArgsException);
    }

    TEST(parseTag, EmptyComponents)
    {
        // Invalid: empty organization
        EXPECT_THROW(
            RegistryUtils::parseTag("/dataset"),
            InvalidArgsException);

        // Invalid: empty dataset
        EXPECT_THROW(
            RegistryUtils::parseTag("org/"),
            InvalidArgsException);

        // Invalid: both empty
        EXPECT_THROW(
            RegistryUtils::parseTag("/"),
            InvalidArgsException);
    }

    TEST(parseTag, EdgeCases)
    {
        // Valid: all lowercase letters
        auto s = RegistryUtils::parseTag("abcdefghijklmnopqrstuvwxyz/dataset");
        EXPECT_STREQ(s.organization.c_str(), "abcdefghijklmnopqrstuvwxyz");

        // Valid: all digits
        s = RegistryUtils::parseTag("0123456789/dataset");
        EXPECT_STREQ(s.organization.c_str(), "0123456789");

        // Valid: underscores and dashes after first char
        s = RegistryUtils::parseTag("a______/b------");
        EXPECT_STREQ(s.organization.c_str(), "a______");
        EXPECT_STREQ(s.dataset.c_str(), "b------");

        // Valid: mixed with numbers
        s = RegistryUtils::parseTag("a1b2c3/d4e5f6");
        EXPECT_STREQ(s.organization.c_str(), "a1b2c3");
        EXPECT_STREQ(s.dataset.c_str(), "d4e5f6");
    }

}
