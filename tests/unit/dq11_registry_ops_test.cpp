/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-11: the operations over a Registry (push, pull, clone, the share upload and the login
// they run behind) against a fake IHttpClient. Happy paths, and the failures that matter: a
// push that dies half way, a 401 (asks for credentials), a 403 (does not), a Registry that
// cannot be reached. No network, no sleeping, no prompt, no user profile.

#include <fstream>
#include <sstream>
#include <string>

#include "dbops.h"
#include "delta_apply.h"
#include "exceptions.h"
#include "fake_http_client.h"
#include "gtest/gtest.h"
#include "mio.h"
#include "mzip.h"
#include "pushmanager.h"
#include "registry_auth.h"
#include "registry_ops.h"
#include "shareclient.h"
#include "shareservice.h"
#include "syncmanager.h"
#include "tagmanager.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;
    using fake::jsonReply;
    using fake::problem;
    using fake::reply;
    using fake::transportFailure;

    constexpr const char *Url = "https://registry.test";
    constexpr const char *Tag = "https://registry.test/org/ds";

    std::string slurp(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Answers with a queue of credentials, and counts how often it was asked
    class ScriptedCredentials : public CredentialProvider
    {
        std::vector<AuthCredentials> answers;

    public:
        int asked = 0;
        std::vector<std::string> askedFor;
        bool refuse = false;

        explicit ScriptedCredentials(std::vector<AuthCredentials> script = {}) : answers(std::move(script)) {}

        AuthCredentials credentialsFor(const std::string &registryUrl) override
        {
            asked++;
            askedFor.push_back(registryUrl);
            if (refuse)
                throw AuthException("not interactive");
            if (answers.empty())
                return AuthCredentials();
            auto next = answers.front();
            if (answers.size() > 1)
                answers.erase(answers.begin());
            return next;
        }
    };

    // The Registry of a test: a fake network that accepts alice/pw
    struct RegistryFixture
    {
        TestArea ta;
        fake::Network net;

        explicit RegistryFixture(const std::string &name) : ta(name, true), net(ta.getPath("auth.json"))
        {
            net.http.on("POST", "/users/authenticate", jsonReply({{"token", "tok-1"}, {"expires", net.now + 3600}}));
        }

        // Logs in as alice: the client of the tests below
        RegistryClient loggedIn()
        {
            RegistryClient client(Url, true, net.deps());
            client.login("alice", "pw");
            net.http.calls.clear();
            return client;
        }
    };

    // =============================================================== withAuthenticatedRegistry

    struct AuthFixture : RegistryFixture
    {
        int runs = 0;
        explicit AuthFixture(const std::string &name) : RegistryFixture(name) {}

        void run(CredentialProvider &provider, const std::function<void(RegistryClient &)> &fn,
                 const AuthenticationOptions &options = {})
        {
            withAuthenticatedRegistry(Url, true, provider, [&](RegistryClient &c) { runs++; fn(c); }, options, net.deps());
        }
    };

    TEST(Dq11WithAuthenticatedRegistry, SavedCredentialsLogInWithoutAsking)
    {
        AuthFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider;
        f.run(provider, [&](RegistryClient &c) { EXPECT_EQ(c.getAuthToken(), "tok-1"); });
        EXPECT_EQ(provider.asked, 0);
        EXPECT_EQ(f.runs, 1);
        EXPECT_EQ(f.net.http.callsTo("POST", "/users/authenticate").size(), 1u);
    }

    TEST(Dq11WithAuthenticatedRegistry, WithoutSavedCredentialsTheProviderIsAskedOnceAndTheyAreSaved)
    {
        AuthFixture f(TEST_NAME);
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        f.run(provider, [](RegistryClient &) {});
        EXPECT_EQ(provider.asked, 1);
        EXPECT_EQ(provider.askedFor, std::vector<std::string>{Url});
        const auto saved = AuthManager(f.ta.getPath("auth.json")).loadCredentials(Url);
        EXPECT_EQ(saved.username, "alice");
        EXPECT_EQ(saved.password, "pw");
    }

    // The password changed since it was saved: one more try, with what the provider gives
    TEST(Dq11WithAuthenticatedRegistry, RefusedSavedCredentialsAreReplacedByTheProviders)
    {
        AuthFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", {problem(401, "Unauthorized"), jsonReply({{"token", "tok-2"}, {"expires", f.net.now + 3600}})});
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "old"));
        ScriptedCredentials provider({AuthCredentials("alice", "new")});
        f.run(provider, [](RegistryClient &) {});
        EXPECT_EQ(provider.asked, 1);
        EXPECT_EQ(f.net.http.callsTo("POST", "/users/authenticate").size(), 2u);
        EXPECT_EQ(AuthManager(f.ta.getPath("auth.json")).loadCredentials(Url).password, "new");
        EXPECT_EQ(f.runs, 1); // the operation ran once: the refusal came before it
    }

    // What the provider gave was wrong too: the user is asked a second time, no more
    TEST(Dq11WithAuthenticatedRegistry, ANewLoginIsAttemptedOnceMoreThenTheRefusalLeaves)
    {
        AuthFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", problem(401, "Unauthorized", "Invalid credentials"));
        ScriptedCredentials provider({AuthCredentials("alice", "x")});
        try
        {
            f.run(provider, [](RegistryClient &) {});
            FAIL();
        }
        catch (const AuthException &e)
        {
            EXPECT_STREQ(e.what(), "Login failed: Invalid credentials");
        }
        EXPECT_EQ(provider.asked, 2); // nothing saved: asked for the first login and for the second
        EXPECT_EQ(f.runs, 0);
    }

    TEST(Dq11WithAuthenticatedRegistry, A401OfTheOperationAsksAgainAndRunsItAgain)
    {
        AuthFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        f.run(provider, [&](RegistryClient &) {
            if (f.runs == 1)
                throw AuthException("Unauthorized");
        });
        EXPECT_EQ(f.runs, 2);
        EXPECT_EQ(provider.asked, 1);
        EXPECT_EQ(f.net.http.callsTo("POST", "/users/authenticate").size(), 2u); // a new login for the new run
    }

    TEST(Dq11WithAuthenticatedRegistry, A401TheSecondTimeLeaves)
    {
        AuthFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        EXPECT_THROW(f.run(provider, [](RegistryClient &) { throw AuthException("Unauthorized"); }), AuthException);
        EXPECT_EQ(f.runs, 2);
        EXPECT_EQ(provider.asked, 1);
    }

    // A 403 is an answer, not a missing login; a transport error is not a refusal either
    TEST(Dq11WithAuthenticatedRegistry, OtherErrorsNeverAsk)
    {
        AuthFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        EXPECT_THROW(f.run(provider, [](RegistryClient &c) { c.expectOk(problem(403, "Forbidden", "No access")); }),
                     RegistryException);
        EXPECT_THROW(f.run(provider, [](RegistryClient &c) { c.expectOk(transportFailure()); }), RegistryException);
        EXPECT_THROW(f.run(provider, [](RegistryClient &) { throw IndexException("no tag"); }), IndexException);
        EXPECT_EQ(provider.asked, 0);
        EXPECT_EQ(f.runs, 3); // each ran once
    }

    // A refused connection is not a refused password: the user is not asked for another one
    TEST(Dq11WithAuthenticatedRegistry, AnUnreachableRegistryIsNotARefusalAndNeverAsksAgain)
    {
        AuthFixture f(TEST_NAME);
        f.net.http.on("POST", "/users/authenticate", transportFailure());
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});

        try
        {
            f.run(provider, [](RegistryClient &) {});
            FAIL() << "the operation ran against an unreachable registry";
        }
        catch (const RegistryException &e)
        {
            EXPECT_STREQ(e.what(), "Login failed: could not connect to https://registry.test: Connection refused");
        }
        EXPECT_EQ(provider.asked, 0);
        EXPECT_EQ(f.runs, 0);
        EXPECT_EQ(f.net.http.callsTo("POST", "/users/authenticate").size(), 3u); // one login, tried three times

        // Nothing saved: the one login needs the credentials once, and a second question is not asked
        AuthFixture g(TEST_NAME + std::string("-unsaved"));
        g.net.http.on("POST", "/users/authenticate", transportFailure(HttpTransportError::Other, "Operation timed out"));
        ScriptedCredentials typed({AuthCredentials("alice", "pw")});
        EXPECT_THROW(g.run(typed, [](RegistryClient &) {}), RegistryException);
        EXPECT_EQ(typed.asked, 1);
    }

    TEST(Dq11WithAuthenticatedRegistry, AProviderThatCannotAskStopsTheOperationBeforeAnyRequest)
    {
        AuthFixture f(TEST_NAME);
        ScriptedCredentials provider;
        provider.refuse = true;
        EXPECT_THROW(f.run(provider, [](RegistryClient &) {}), AuthException);
        EXPECT_EQ(provider.asked, 1);
        EXPECT_EQ(f.runs, 0);
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    TEST(Dq11WithAuthenticatedRegistry, EmptyCredentialsFromTheProviderAreNotSent)
    {
        AuthFixture f(TEST_NAME);
        ScriptedCredentials provider; // answers nothing
        try
        {
            f.run(provider, [](RegistryClient &) {});
            FAIL();
        }
        catch (const AuthException &e)
        {
            EXPECT_STREQ(e.what(), "Cannot authenticate with https://registry.test: no credentials were given");
        }
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    // A public dataset is read without a login; the credentials come when the server says 401
    TEST(Dq11WithAuthenticatedRegistry, AnonymousFirstRunsWithoutALogin)
    {
        AuthFixture f(TEST_NAME);
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        AuthenticationOptions options;
        options.tryAnonymousFirst = true;
        f.run(provider, [](RegistryClient &c) { EXPECT_TRUE(c.getAuthToken().empty()); }, options);
        EXPECT_EQ(provider.asked, 0);
        EXPECT_TRUE(f.net.http.callsTo("POST", "/users/authenticate").empty());
    }

    TEST(Dq11WithAuthenticatedRegistry, AnonymousFirstThen401AsksAndRunsAgainLoggedIn)
    {
        AuthFixture f(TEST_NAME);
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        AuthenticationOptions options;
        options.tryAnonymousFirst = true;
        f.run(provider,
              [&](RegistryClient &c) {
                  if (f.runs == 1)
                      throw AuthException("Unauthorized");
                  EXPECT_EQ(c.getAuthToken(), "tok-1");
              },
              options);
        EXPECT_EQ(f.runs, 2);
        EXPECT_EQ(provider.asked, 1);
    }

    TEST(Dq11WithAuthenticatedRegistry, AnonymousFirstStillUsesSavedCredentials)
    {
        AuthFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider;
        AuthenticationOptions options;
        options.tryAnonymousFirst = true;
        f.run(provider, [](RegistryClient &c) { EXPECT_EQ(c.getAuthToken(), "tok-1"); }, options);
        EXPECT_EQ(provider.asked, 0);
    }

    // =============================================================== push

    struct PushFixture : RegistryFixture
    {
        IndexFixture ds;

        // `synced`: the dataset was pushed or pulled before (it has the stamp of that sync)
        explicit PushFixture(const std::string &name, bool tagged = true, bool synced = true)
            : RegistryFixture(name), ds(name + "-ds", "ds", true)
        {
            writeFile(ds.root / "a.txt", "alpha");
            writeFile(ds.root / "sub" / "b.txt", "beta beta");
            addToIndex(ds.db.get(), {(ds.root / "a.txt").string(), (ds.root / "sub").string()});
            if (tagged)
                TagManager(ds.db.get()).setTag(Tag);
            // A stamp that is not the dataset's: what the last push left
            if (synced)
                SyncManager(ds.db.get()).setLastStamp(Url, json{{"checksum", "last-push"}, {"entries", json::array()}});
            ds.db->close();
        }

        void serve(const std::vector<std::string> &needed, bool existing = true)
        {
            net.http.on("GET", "/orgs/org/ds/ds",
                        existing ? jsonReply(json::array({{{"path", ""}, {"hash", ""}, {"type", 1}, {"size", 0}, {"depth", 0}, {"mtime", 1}}}))
                                 : problem(404, "Not Found"));
            net.http.on("POST", "/push/init", jsonReply({{"neededFiles", needed}, {"neededMeta", json::array()}, {"token", "push-token"}}));
            net.http.on("MULTIPART", "/push/upload", reply(200));
            net.http.on("POST", "/push/commit", reply(200));
        }

        std::string lastChecksum()
        {
            auto db = ddb::open(ds.root.string(), false);
            return SyncManager(db.get()).getLastStamp(Url)["checksum"];
        }
        std::string datasetChecksum()
        {
            auto db = ddb::open(ds.root.string(), false);
            return db->getStampChecksum();
        }
    };

    TEST(Dq11Push, SendsInitTheNeededFilesAndTheCommitInOrder)
    {
        PushFixture f(TEST_NAME);
        f.serve({"a.txt", "sub/b.txt"});
        auto client = f.loggedIn();
        std::ostringstream out;

        pushDataset(client, f.ds.root.string(), out);

        EXPECT_TRUE(f.net.http.unexpected.empty());
        std::vector<std::string> sequence;
        for (const auto &c : f.net.http.calls)
            sequence.push_back(c.method + " " + c.path.substr(std::string(Url).size()));
        EXPECT_EQ(sequence, (std::vector<std::string>{"GET /orgs/org/ds/ds", "POST /orgs/org/ds/ds/push/init",
                                                      "MULTIPART /orgs/org/ds/ds/push/upload",
                                                      "MULTIPART /orgs/org/ds/ds/push/upload",
                                                      "POST /orgs/org/ds/ds/push/commit"}));

        const auto init = f.net.http.callsTo("POST", "/push/init").at(0);
        EXPECT_EQ(init.header("Authorization"), "Bearer tok-1");
        EXPECT_EQ(init.field("checksum"), "last-push");
        EXPECT_EQ(json::parse(init.field("stamp"))["checksum"], f.datasetChecksum());

        const auto uploads = f.net.http.callsTo("MULTIPART", "/push/upload");
        EXPECT_EQ(uploads.at(0).part("path"), "a.txt");
        EXPECT_EQ(uploads.at(0).part("file"), "alpha");
        EXPECT_EQ(uploads.at(0).part("token"), "push-token");
        EXPECT_EQ(uploads.at(1).part("path"), "sub/b.txt");
        EXPECT_EQ(uploads.at(1).part("file"), "beta beta");
        EXPECT_EQ(uploads.at(0).header("Authorization"), "Bearer tok-1");

        EXPECT_EQ(f.net.http.callsTo("POST", "/push/commit").at(0).field("token"), "push-token");

        EXPECT_EQ(out.str(), "Pushing to 'https://registry.test/org/ds'\nTransfering 'a.txt'\nTransfering 'sub/b.txt'\nPush completed\n");
        EXPECT_EQ(f.lastChecksum(), f.datasetChecksum()); // the push is recorded
    }

    TEST(Dq11Push, ANewDatasetIsPushedAsNew)
    {
        PushFixture f(TEST_NAME);
        f.serve({"a.txt"}, false);
        auto client = f.loggedIn();
        std::ostringstream out;
        pushDataset(client, f.ds.root.string(), out);
        EXPECT_NE(out.str().find("Pushing to new 'https://registry.test/org/ds'"), std::string::npos) << out.str();
        EXPECT_EQ(f.net.http.callsTo("MULTIPART", "/push/upload").size(), 1u);
    }

    TEST(Dq11Push, NothingNeededIsJustInitAndCommit)
    {
        PushFixture f(TEST_NAME);
        f.serve({});
        auto client = f.loggedIn();
        std::ostringstream out;
        pushDataset(client, f.ds.root.string(), out);
        EXPECT_TRUE(f.net.http.callsTo("MULTIPART", "/push/upload").empty());
        EXPECT_EQ(f.net.http.callsTo("POST", "/push/commit").size(), 1u);
    }

    // The push that dies half way: the second upload fails. Nothing is committed, nothing is sent
    // twice, and the dataset does not record a push that did not happen.
    TEST(Dq11Push, AFailureHalfWayCommitsNothingAndIsNotTriedAgain)
    {
        PushFixture f(TEST_NAME);
        f.serve({"a.txt", "sub/b.txt"});
        f.net.http.on("MULTIPART", "/push/upload", {reply(200), problem(500, "Internal Server Error", "disk full")});
        auto client = f.loggedIn();
        std::ostringstream out;

        try
        {
            pushDataset(client, f.ds.root.string(), out);
            FAIL();
        }
        catch (const RegistryException &e)
        {
            EXPECT_STREQ(e.what(), "Error response from registry: disk full");
        }

        EXPECT_EQ(f.net.http.callsTo("MULTIPART", "/push/upload").size(), 2u); // one attempt per file
        EXPECT_TRUE(f.net.http.callsTo("POST", "/push/commit").empty());
        EXPECT_TRUE(f.net.sleeps.empty());
        EXPECT_EQ(f.lastChecksum(), "last-push");
        EXPECT_EQ(out.str().find("Push completed"), std::string::npos);
    }

    TEST(Dq11Push, ALostConnectionDuringAnUploadIsReportedAndNotTriedAgain)
    {
        PushFixture f(TEST_NAME);
        f.serve({"a.txt", "sub/b.txt"});
        f.net.http.on("MULTIPART", "/push/upload",
                      {reply(200), transportFailure(HttpTransportError::Other, "Connection reset by peer")});
        auto client = f.loggedIn();
        std::ostringstream out;
        try
        {
            pushDataset(client, f.ds.root.string(), out);
            FAIL();
        }
        catch (const RegistryException &e)
        {
            EXPECT_STREQ(e.what(),
                         "Registry request failed: request to https://registry.test failed: Connection reset by peer");
        }
        EXPECT_EQ(f.net.http.callsTo("MULTIPART", "/push/upload").size(), 2u);
        EXPECT_TRUE(f.net.http.callsTo("POST", "/push/commit").empty());
    }

    TEST(Dq11Push, TheRegistryHasChangesThatMustBePulledFirst)
    {
        PushFixture f(TEST_NAME);
        f.serve({});
        f.net.http.on("POST", "/push/init", jsonReply({{"pullRequired", true}}));
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pushDataset(client, f.ds.root.string(), out), PullRequiredException);
        EXPECT_TRUE(f.net.http.callsTo("MULTIPART", "/push/upload").empty());
    }

    TEST(Dq11Push, A403IsAnErrorOfTheUserNotOfTheLogin)
    {
        PushFixture f(TEST_NAME);
        f.serve({});
        f.net.http.on("POST", "/push/init", problem(403, "Forbidden", "You cannot push to this dataset"));
        auto client = f.loggedIn();
        std::ostringstream out;
        try
        {
            pushDataset(client, f.ds.root.string(), out);
            FAIL();
        }
        catch (const AuthException &)
        {
            FAIL() << "a 403 is not an AuthException";
        }
        catch (const RegistryException &e)
        {
            EXPECT_STREQ(e.what(), "Error response from registry: You cannot push to this dataset");
        }
    }

    TEST(Dq11Push, A401IsAnAuthError)
    {
        PushFixture f(TEST_NAME);
        f.serve({});
        f.net.http.on("POST", "/push/init", problem(401, "Unauthorized"));
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pushDataset(client, f.ds.root.string(), out), AuthException);
    }

    TEST(Dq11Push, AnAnswerWithoutTheNeededFilesIsInvalid)
    {
        PushFixture f(TEST_NAME);
        f.serve({});
        f.net.http.on("POST", "/push/init", reply(200, R"({"token":"t"})"));
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pushDataset(client, f.ds.root.string(), out), RegistryException);
    }

    // Nothing was ever synced: the Registry is told the dataset it last saw was empty, not that
    // it has the dataset's own checksum (which is true of no Registry)
    TEST(Dq11Push, TheFirstPushNamesTheEmptyDatasetAsTheStampOfTheRegistry)
    {
        PushFixture f(TEST_NAME, true, false);
        f.serve({"a.txt", "sub/b.txt"});
        auto client = f.loggedIn();
        std::ostringstream out;

        pushDataset(client, f.ds.root.string(), out);

        const auto init = f.net.http.callsTo("POST", "/push/init").at(0);
        EXPECT_EQ(init.field("checksum"), Database::emptyStamp()["checksum"].get<std::string>());
        EXPECT_NE(init.field("checksum"), f.datasetChecksum());
        EXPECT_EQ(json::parse(init.field("stamp"))["checksum"], f.datasetChecksum()); // what is pushed is the dataset

        // and it is the dataset that was pushed which the next push is based on
        EXPECT_EQ(f.lastChecksum(), f.datasetChecksum());
    }

    // A pull is required, with its own message, when the Registry has content of its own
    TEST(Dq11Push, AFirstPushToADatasetWithContentIsToldToPullFirst)
    {
        PushFixture f(TEST_NAME, true, false);
        f.serve({});
        f.net.http.on("POST", "/push/init", jsonReply({{"pullRequired", true}}));
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pushDataset(client, f.ds.root.string(), out), PullRequiredException);
        EXPECT_TRUE(f.net.http.callsTo("MULTIPART", "/push/upload").empty());
    }

    TEST(Dq11Push, ADatasetWithoutATagCannotBePushed)
    {
        PushFixture f(TEST_NAME, false);
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pushDataset(client, f.ds.root.string(), out), IndexException);
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    TEST(Dq11PushManager, TheMetaRequestIsAFormWithTheToken)
    {
        RegistryFixture f(TEST_NAME);
        f.net.http.on("POST", "/orgs/o/ds/d/push/meta", reply(200));
        auto client = f.loggedIn();
        PushManager pm(&client, "o", "d");
        pm.meta(json::array({{{"id", "1"}}}), "push-token");
        const auto &call = f.net.http.calls.at(0);
        EXPECT_EQ(call.header("Authorization"), "Bearer tok-1");
        EXPECT_EQ(call.field("token"), "push-token");
        EXPECT_EQ(json::parse(call.field("meta")).size(), 1u);
        EXPECT_EQ(pm.getOrganization(), "o");
        EXPECT_EQ(pm.getDataset(), "d");
    }

    // =============================================================== pull / clone

    struct PullFixture : RegistryFixture
    {
        IndexFixture remote;
        IndexFixture local;
        json remoteStamp;

        // `synced`: the local dataset was pulled before, and has not changed since
        explicit PullFixture(const std::string &name, const std::map<std::string, std::string> &remoteFiles,
                             const std::map<std::string, std::string> &localFiles = {}, bool tagged = true,
                             bool synced = true)
            : RegistryFixture(name), remote(name, "remote", true), local(name, "local", false)
        {
            std::vector<std::string> paths;
            for (const auto &[p, content] : remoteFiles)
            {
                writeFile(remote.root / p, content);
                paths.push_back((remote.root / p).string());
            }
            if (!paths.empty())
                addToIndex(remote.db.get(), paths);
            remoteStamp = remote.db->getStamp();

            paths.clear();
            for (const auto &[p, content] : localFiles)
            {
                writeFile(local.root / p, content);
                paths.push_back((local.root / p).string());
            }
            if (!paths.empty())
                addToIndex(local.db.get(), paths);
            if (tagged)
                TagManager(local.db.get()).setTag(Tag);
            // The stamp of the last pull: the dataset as it is
            if (synced)
                SyncManager(local.db.get()).setLastStamp(Url, local.db.get());
            local.db->close();

            net.http.on("GET", "/orgs/org/ds/ds/stamp", jsonReply(remoteStamp));
        }
    };

    TEST(Dq11Pull, DownloadsOneNewFileWithAGetAndAppliesIt)
    {
        PullFixture f(TEST_NAME, {{"r1.txt", "remote one"}});
        f.net.http.on("DOWNLOAD", "/orgs/org/ds/ds/download?path=r1.txt", reply(200, "remote one"));
        auto client = f.loggedIn();
        std::ostringstream out;

        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);

        EXPECT_TRUE(f.net.http.unexpected.empty());
        EXPECT_EQ(slurp(f.local.root / "r1.txt"), "remote one");
        EXPECT_EQ(f.net.http.callsTo("DOWNLOAD", "path=r1.txt").at(0).header("Cookie"), "jwtToken=tok-1");
        EXPECT_NE(out.str().find("Pulling from 'https://registry.test/org/ds'"), std::string::npos) << out.str();
        EXPECT_NE(out.str().find("A\tr1.txt"), std::string::npos) << out.str();
        EXPECT_NE(out.str().find("Pull completed"), std::string::npos) << out.str();

        auto db = ddb::open(f.local.root.string(), false);
        EXPECT_TRUE(getEntry(db.get(), "r1.txt").has_value()); // indexed
        EXPECT_EQ(SyncManager(db.get()).getLastStamp(Url)["checksum"], f.remoteStamp["checksum"]);
        EXPECT_TRUE(fs::is_empty(db->ddbDirectory() / "tmp" / "pull_cache") || !fs::exists(db->ddbDirectory() / "tmp" / "pull_cache" / "org-ds"));
    }

    // The first pull has no stamp of a previous one: the baseline is the empty dataset, not the
    // local one. With the local one, a file the Registry does not have was a file the Registry
    // deleted, and the first pull deleted it from the disk.
    TEST(Dq11Pull, TheFirstPullKeepsTheLocalFilesTheRegistryDoesNotHave)
    {
        PullFixture f(TEST_NAME, {}, {{"mine.txt", "mine"}}, true, false);
        auto client = f.loggedIn();
        std::ostringstream out;

        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);

        EXPECT_EQ(slurp(f.local.root / "mine.txt"), "mine");
        EXPECT_EQ(out.str().find("D\tmine.txt"), std::string::npos) << out.str();
        auto db = ddb::open(f.local.root.string(), false);
        EXPECT_TRUE(getEntry(db.get(), "mine.txt").has_value());
        EXPECT_EQ(SyncManager(db.get()).getLastStamp(Url)["checksum"], f.remoteStamp["checksum"]);
    }

    TEST(Dq11Pull, TheFirstPullAddsTheRemoteFilesAndKeepsTheLocalOnes)
    {
        PullFixture f(TEST_NAME, {{"r1.txt", "remote one"}}, {{"mine.txt", "mine"}}, true, false);
        f.net.http.on("DOWNLOAD", "/orgs/org/ds/ds/download?path=r1.txt", reply(200, "remote one"));
        auto client = f.loggedIn();
        std::ostringstream out;

        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);

        EXPECT_EQ(slurp(f.local.root / "r1.txt"), "remote one");
        EXPECT_EQ(slurp(f.local.root / "mine.txt"), "mine");
    }

    // Both sides have the path and the content differs: nobody deleted or changed it since a
    // sync (there was none), so it is a conflict for the user to settle, not an overwrite
    TEST(Dq11Pull, TheFirstPullReportsAFileBothSidesHaveAsAConflict)
    {
        PullFixture f(TEST_NAME, {{"both.txt", "theirs"}}, {{"both.txt", "ours"}}, true, false);
        f.net.http.on("DOWNLOAD", "/orgs/org/ds/ds/download?path=both.txt", reply(200, "theirs"));
        auto client = f.loggedIn();
        std::ostringstream out;

        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);

        EXPECT_EQ(slurp(f.local.root / "both.txt"), "ours");
        EXPECT_NE(out.str().find("C\tboth.txt"), std::string::npos) << out.str();
        auto db = ddb::open(f.local.root.string(), false);
        EXPECT_NE(SyncManager(db.get()).getLastStamp(Url)["checksum"], f.remoteStamp["checksum"]); // not recorded: conflicts
    }

    TEST(Dq11Pull, SameFilesOnBothSidesAreNotAConflict)
    {
        PullFixture f(TEST_NAME, {{"same.txt", "same"}}, {{"same.txt", "same"}}, true, false);
        auto client = f.loggedIn();
        std::ostringstream out;

        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);

        EXPECT_EQ(out.str().find("C\t"), std::string::npos) << out.str();
        EXPECT_TRUE(f.net.http.callsTo("DOWNLOAD", "same.txt").empty());
        auto db = ddb::open(f.local.root.string(), false);
        EXPECT_EQ(SyncManager(db.get()).getLastStamp(Url)["checksum"], f.remoteStamp["checksum"]);
    }

    TEST(Dq11Pull, SeveralNewFilesComeInOneArchive)
    {
        PullFixture f(TEST_NAME, {{"r1.txt", "one"}, {"r2.txt", "two two"}});
        const auto zipFile = f.remote.ta.getPath("remote.zip");
        // The archive the Registry would build for the two paths
        const auto staging = f.remote.ta.getFolder("staging");
        writeFile(staging / "r1.txt", "one");
        writeFile(staging / "r2.txt", "two two");
        ddb::zip::zipFolder(io::toUtf8(staging), io::toUtf8(zipFile), {});
        f.net.http.on("DOWNLOAD-POST", "/orgs/org/ds/ds/download", reply(200, slurp(zipFile)));
        auto client = f.loggedIn();
        std::ostringstream out;

        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);

        EXPECT_EQ(slurp(f.local.root / "r1.txt"), "one");
        EXPECT_EQ(slurp(f.local.root / "r2.txt"), "two two");
        EXPECT_EQ(f.net.http.callsTo("DOWNLOAD-POST", "/download").at(0).field("path"), "r1.txt,r2.txt");
    }

    TEST(Dq11Pull, AFileTheDatasetAlreadyHasIsNotDownloaded)
    {
        PullFixture f(TEST_NAME, {{"same.txt", "same"}, {"new.txt", "new"}}, {{"copy.txt", "same"}});
        f.net.http.on("DOWNLOAD", "/orgs/org/ds/ds/download?path=new.txt", reply(200, "new"));
        auto client = f.loggedIn();
        std::ostringstream out;
        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);
        EXPECT_EQ(slurp(f.local.root / "same.txt"), "same");
        EXPECT_EQ(slurp(f.local.root / "new.txt"), "new");
        EXPECT_EQ(f.net.http.callsTo("DOWNLOAD", "/download?path=same.txt").size(), 0u); // linked from copy.txt
        EXPECT_EQ(f.net.http.callsTo("DOWNLOAD", "/download?path=new.txt").size(), 1u);
    }

    TEST(Dq11Pull, NothingNewIsUpToDate)
    {
        PullFixture f(TEST_NAME, {});
        auto client = f.loggedIn();
        std::ostringstream out;
        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);
        EXPECT_NE(out.str().find("Everything up-to-date"), std::string::npos) << out.str();
        EXPECT_EQ(f.net.http.calls.size(), 1u); // the stamp
    }

    TEST(Dq11Pull, AFailedDownloadChangesNothingAndDoesNotRecordAPull)
    {
        PullFixture f(TEST_NAME, {{"r1.txt", "remote one"}});
        f.net.http.on("DOWNLOAD", "/download?path=r1.txt", problem(500, "Internal Server Error", "storage offline"));
        auto client = f.loggedIn();
        std::ostringstream out;
        try
        {
            pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);
            FAIL();
        }
        catch (const RegistryException &e)
        {
            EXPECT_STREQ(e.what(), "Error response from registry: storage offline");
        }
        EXPECT_FALSE(fs::exists(f.local.root / "r1.txt"));
        auto db = ddb::open(f.local.root.string(), false);
        EXPECT_NE(SyncManager(db.get()).getLastStamp(Url)["checksum"], f.remoteStamp["checksum"]);
    }

    TEST(Dq11Pull, TheStampIsReadWithAGetThatIsTriedAgainOn5xx)
    {
        PullFixture f(TEST_NAME, {});
        f.net.http.on("GET", "/orgs/org/ds/ds/stamp", {problem(503, "Busy"), jsonReply(f.remoteStamp)});
        auto client = f.loggedIn();
        std::ostringstream out;
        pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out);
        EXPECT_EQ(f.net.http.callsTo("GET", "/stamp").size(), 2u);
        EXPECT_EQ(f.net.sleeps.size(), 1u);
    }

    TEST(Dq11Pull, A401OnTheStampIsAnAuthError)
    {
        PullFixture f(TEST_NAME, {});
        f.net.http.on("GET", "/orgs/org/ds/ds/stamp", problem(401, "Unauthorized"));
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out), AuthException);
    }

    TEST(Dq11Pull, ADatasetWithoutATagCannotBePulled)
    {
        PullFixture f(TEST_NAME, {}, {}, false);
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(pullDataset(client, f.local.root.string(), MergeStrategy::DontMerge, out), IndexException);
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    TEST(Dq11Clone, IndexesTagsAndPullsIntoAnEmptyFolder)
    {
        PullFixture f(TEST_NAME, {{"r1.txt", "remote one"}});
        f.net.http.on("DOWNLOAD", "/download?path=r1.txt", reply(200, "remote one"));
        auto client = f.loggedIn();
        const auto target = f.local.ta.getPath("cloned");
        std::ostringstream out;

        cloneDataset(client, "org", "ds", io::toUtf8(target), out);

        EXPECT_EQ(slurp(target / "r1.txt"), "remote one");
        auto db = ddb::open(io::toUtf8(target), false);
        EXPECT_EQ(TagManager(db.get()).getTag(), "https://registry.test/org/ds");
    }

    TEST(Dq11Clone, ANonEmptyFolderIsRefusedBeforeAnyRequest)
    {
        PullFixture f(TEST_NAME, {{"r1.txt", "x"}});
        auto client = f.loggedIn();
        std::ostringstream out;
        EXPECT_THROW(cloneDataset(client, "org", "ds", io::toUtf8(f.local.root), out), FSException);
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    // =============================================================== share

    struct ShareFixture : RegistryFixture
    {
        fs::path folder;

        explicit ShareFixture(const std::string &name) : RegistryFixture(name)
        {
            folder = ta.getFolder("files");
            writeFile(folder / "a.txt", "alpha");
            writeFile(folder / "sub" / "b.txt", "beta beta");
            net.http.on("POST", "/share/init", jsonReply({{"token", "share-token"}}));
            net.http.on("MULTIPART", "/share/upload/share-token", jsonReply({{"hash", "h"}}));
            net.http.on("POST", "/share/commit/share-token", jsonReply({{"url", "/r/share-token"}}));
        }
    };

    TEST(Dq11ShareClient, InitUploadCommit)
    {
        ShareFixture f(TEST_NAME);
        auto client = f.loggedIn();
        ShareClient share(&client);

        share.Init("org/ds", "secret");
        EXPECT_EQ(share.getToken(), "share-token");
        const auto init = f.net.http.callsTo("POST", "/share/init").at(0);
        EXPECT_EQ(init.header("Authorization"), "Bearer tok-1");
        EXPECT_EQ(init.field("tag"), "org/ds");
        EXPECT_EQ(init.field("password"), "secret");

        std::vector<std::pair<size_t, size_t>> progress;
        f.net.http.progressEvents = {HttpProgress{0, 0, 100, 40}};
        share.Upload("sub/b.txt", f.folder / "sub" / "b.txt", [&](std::string &name, size_t tx, size_t total) {
            EXPECT_EQ(name, "b.txt");
            progress.emplace_back(tx, total);
            return true;
        });
        const auto up = f.net.http.callsTo("MULTIPART", "/share/upload/share-token").at(0);
        EXPECT_EQ(up.part("path"), "sub/b.txt");
        EXPECT_EQ(up.part("file"), "beta beta");
        // A large file on a slow link must get through: no cap on the whole transfer, only on
        // opening the connection and on a transfer that stopped moving
        EXPECT_EQ(up.request.limits.total, std::chrono::milliseconds(0));
        EXPECT_GT(up.request.limits.connect, std::chrono::milliseconds(0));
        EXPECT_GT(up.request.limits.stall, std::chrono::milliseconds(0));
        EXPECT_EQ(up.header("Authorization"), "Bearer tok-1");
        ASSERT_EQ(progress.size(), 1u);
        EXPECT_EQ(progress[0], std::make_pair(size_t(40), size_t(100)));

        EXPECT_EQ(share.Commit(), "https://registry.test/r/share-token");
        EXPECT_EQ(share.getResultUrl(), "https://registry.test/r/share-token");
        EXPECT_TRUE(f.net.http.callsTo("POST", "/share/commit/share-token").at(0).request.form.empty());
    }

    TEST(Dq11ShareClient, UploadAndCommitNeedInit)
    {
        ShareFixture f(TEST_NAME);
        auto client = f.loggedIn();
        ShareClient share(&client);
        EXPECT_THROW(share.Upload("a.txt", f.folder / "a.txt"), InvalidArgsException);
        EXPECT_THROW(share.Commit(), InvalidArgsException);
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    TEST(Dq11ShareClient, ErrorsOfEachStep)
    {
        {
            ShareFixture f(std::string(TEST_NAME) + "A");
            f.net.http.on("POST", "/share/init", problem(401, "Unauthorized"));
            auto client = f.loggedIn();
            ShareClient share(&client);
            EXPECT_THROW(share.Init("org/ds", ""), AuthException);
        }
        {
            ShareFixture f(std::string(TEST_NAME) + "B");
            f.net.http.on("POST", "/share/init", reply(200, "{}"));
            auto client = f.loggedIn();
            ShareClient share(&client);
            EXPECT_THROW(share.Init("org/ds", ""), RegistryException);
        }
        {
            ShareFixture f(std::string(TEST_NAME) + "C");
            f.net.http.on("MULTIPART", "/share/upload/share-token", problem(413, "Payload Too Large", "File too large", true));
            auto client = f.loggedIn();
            ShareClient share(&client);
            share.Init("org/ds", "");
            try
            {
                share.Upload("a.txt", f.folder / "a.txt");
                FAIL();
            }
            catch (const RegistryException &e)
            {
                EXPECT_STREQ(e.what(), "Error response from registry: File too large");
            }
        }
        {
            ShareFixture f(std::string(TEST_NAME) + "D");
            f.net.http.on("MULTIPART", "/share/upload/share-token", reply(200, "{}"));
            auto client = f.loggedIn();
            ShareClient share(&client);
            share.Init("org/ds", "");
            EXPECT_THROW(share.Upload("a.txt", f.folder / "a.txt"), RegistryException);
        }
        {
            ShareFixture f(std::string(TEST_NAME) + "E");
            f.net.http.on("POST", "/share/commit/share-token", transportFailure());
            auto client = f.loggedIn();
            ShareClient share(&client);
            share.Init("org/ds", "");
            EXPECT_THROW(share.Commit(), RegistryException);
            EXPECT_EQ(f.net.http.callsTo("POST", "/share/commit/share-token").size(), 1u); // not tried again
        }
    }

    // The request the tests make: the files (as paths under cwd), a Registry tag and a password
    ShareRequest shareOf(std::vector<std::string> input, const std::string &tag, const std::string &cwd = "")
    {
        ShareRequest request;
        request.input = std::move(input);
        request.tag = tag;
        request.cwd = cwd;
        return request;
    }

    TEST(Dq11ShareService, SharesThroughALoginWithTheProvidersCredentials)
    {
        ShareFixture f(TEST_NAME);
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        ShareService service(f.net.deps());

        const auto url = service.share(shareOf({io::toUtf8(f.folder / "a.txt"), io::toUtf8(f.folder / "sub" / "b.txt")},
                                                     "registry.test/org/ds", io::toUtf8(f.folder)),
                                       provider);

        EXPECT_EQ(url, "https://registry.test/r/share-token");
        EXPECT_EQ(provider.asked, 1);
        const auto uploads = f.net.http.callsTo("MULTIPART", "/share/upload/share-token");
        ASSERT_EQ(uploads.size(), 2u);
        EXPECT_EQ(uploads[0].part("path"), "a.txt");
        EXPECT_EQ(uploads[1].part("path"), "sub/b.txt");
        // login, init, two uploads, commit: one login only
        EXPECT_EQ(f.net.http.callsTo("POST", "/users/authenticate").size(), 1u);
        EXPECT_EQ(f.net.http.callsTo("POST", "/share/init").at(0).field("tag"), "org/ds");
    }

    TEST(Dq11ShareService, SavedCredentialsAreUsedWithoutAsking)
    {
        ShareFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        ScriptedCredentials provider;
        ShareService service(f.net.deps());
        service.share(shareOf({io::toUtf8(f.folder / "a.txt")}, "registry.test/org/ds", io::toUtf8(f.folder)), provider);
        EXPECT_EQ(provider.asked, 0);
    }

    // The non-interactive case: nothing is asked, and nothing reaches the server
    TEST(Dq11ShareService, AProviderThatCannotAskStopsTheShareBeforeAnyRequest)
    {
        ShareFixture f(TEST_NAME);
        ScriptedCredentials provider;
        provider.refuse = true;
        ShareService service(f.net.deps());
        EXPECT_THROW(service.share(shareOf({io::toUtf8(f.folder / "a.txt")}, "registry.test/org/ds", io::toUtf8(f.folder)), provider),
                     AuthException);
        EXPECT_TRUE(f.net.http.calls.empty());
    }

    // The token was revoked: the Registry answers 401 to the upload. The credentials are asked
    // for and the share starts again from its init
    TEST(Dq11ShareService, A401InTheMiddleAsksAndSharesAgain)
    {
        ShareFixture f(TEST_NAME);
        f.net.credentials.saveCredentials(Url, AuthCredentials("alice", "pw"));
        f.net.http.on("MULTIPART", "/share/upload/share-token", {problem(401, "Unauthorized"), jsonReply({{"hash", "h"}})});
        ScriptedCredentials provider({AuthCredentials("alice", "pw")});
        ShareService service(f.net.deps());
        const auto url = service.share(shareOf({io::toUtf8(f.folder / "a.txt")}, "registry.test/org/ds", io::toUtf8(f.folder)), provider);
        EXPECT_EQ(url, "https://registry.test/r/share-token");
        EXPECT_EQ(provider.asked, 1);
        EXPECT_EQ(f.net.http.callsTo("POST", "/share/init").size(), 2u);
    }

    TEST(Dq11ShareService, NothingToShareFailsBeforeAnyLogin)
    {
        ShareFixture f(TEST_NAME);
        ScriptedCredentials provider;
        ShareService service(f.net.deps());
        EXPECT_THROW(service.share(shareOf({}, "registry.test/org/ds"), provider), InvalidArgsException);
        EXPECT_THROW(service.share(shareOf({io::toUtf8(f.folder / "missing.txt")}, "registry.test/org/ds", io::toUtf8(f.folder)), provider),
                     InvalidArgsException);
        EXPECT_EQ(provider.asked, 0);
        EXPECT_TRUE(f.net.http.calls.empty());
    }
}
