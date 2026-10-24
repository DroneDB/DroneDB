/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef FAKE_HTTP_CLIENT_H
#define FAKE_HTTP_CLIENT_H

// An IHttpClient that answers from a script (DQ-11), so the Registry code runs without a network:
// a reply is queued for a route (method + the end of the URL) and the requests are recorded, the
// files of a multipart body read at the time of the call. Nothing sleeps: RegistryDeps with a
// recording sleeper and a settable clock are made by FakeNetwork.

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "authmanager.h"
#include "http_client.h"
#include "json.h"
#include "registry_client.h"

namespace fake
{
    // What the client was asked to send
    struct Call
    {
        std::string method; // GET, POST (form), MULTIPART, DOWNLOAD
        ddb::HttpRequest request;
        std::string path;                    // the URL without the base
        std::map<std::string, std::string> filesSent; // multipart file parts: part name -> content
        std::string header(const std::string &name) const
        {
            for (const auto &[k, v] : request.headers)
                if (k == name)
                    return v;
            return "";
        }
        std::string field(const std::string &name) const
        {
            for (const auto &[k, v] : request.form)
                if (k == name)
                    return v;
            return "";
        }
        std::string part(const std::string &name) const
        {
            for (const auto &p : request.parts)
                if (p.name == name)
                    return p.file.empty() ? p.value : filesSent.at(name);
            return "";
        }
    };

    inline ddb::HttpResponse reply(long status, std::string body = "", std::map<std::string, std::string> headers = {})
    {
        ddb::HttpResponse res;
        res.status = status;
        res.body = std::move(body);
        res.headers = std::move(headers);
        return res;
    }

    inline ddb::HttpResponse jsonReply(const nlohmann::json &j, long status = 200)
    {
        return reply(status, j.dump(), {{"content-type", "application/json"}});
    }

    // An RFC 9457 ProblemDetails reply, as the Registry sends it
    inline ddb::HttpResponse problem(long status, const std::string &title, const std::string &detail = "",
                                     bool noRetry = false)
    {
        nlohmann::json j = {{"type", "about:blank"}, {"title", title}, {"status", status}, {"noRetry", noRetry}};
        if (!detail.empty())
            j["detail"] = detail;
        return reply(status, j.dump(), {{"content-type", "application/problem+json"}});
    }

    inline ddb::HttpResponse transportFailure(ddb::HttpTransportError kind = ddb::HttpTransportError::CouldNotConnect,
                                              const std::string &message = "Connection refused")
    {
        ddb::HttpResponse res;
        res.transport = kind;
        res.transportMessage = message;
        return res;
    }

    class HttpClient : public ddb::IHttpClient
    {
        struct Route
        {
            std::string method;
            std::string suffix;
            std::deque<ddb::HttpResponse> replies;
        };
        std::vector<Route> routes;

        ddb::HttpResponse answer(const std::string &method, const ddb::HttpRequest &request, std::ostream *sink)
        {
            Call call;
            call.method = method;
            call.request = request;
            call.path = request.url;
            for (const auto &part : request.parts)
                if (!part.file.empty())
                {
                    std::ifstream f(part.file, std::ios::binary);
                    std::stringstream ss;
                    ss << f.rdbuf();
                    call.filesSent[part.name] = ss.str();
                }
            calls.push_back(call);

            // The progress the transfer reports while it runs (the callback must not be called
            // later: it captures what the caller keeps only for the duration of the call)
            if (request.progress)
                for (const auto &event : progressEvents)
                    request.progress(event);

            // The longest suffix wins, so "/ds/x/push/init" is not taken for "/init"
            Route *best = nullptr;
            for (auto &route : routes)
            {
                if (route.method != method || route.replies.empty())
                    continue;
                const auto &url = request.url;
                if (url.size() >= route.suffix.size() &&
                    url.compare(url.size() - route.suffix.size(), route.suffix.size(), route.suffix) == 0 &&
                    (best == nullptr || route.suffix.size() > best->suffix.size()))
                    best = &route;
            }
            if (best == nullptr)
            {
                unexpected.push_back(method + " " + request.url);
                return reply(404, "no route in the fake");
            }

            auto res = best->replies.front();
            if (best->replies.size() > 1)
                best->replies.pop_front(); // the last reply repeats
            if (sink != nullptr)
            {
                *sink << res.body;
                res.body.clear();
            }
            return res;
        }

    public:
        std::vector<Call> calls;
        std::vector<std::string> unexpected; // requests no route answered
        std::vector<ddb::HttpProgress> progressEvents; // reported to the progress callback of each call

        // The next replies of `method` + a URL that ends with `suffix` (the last one repeats)
        void on(const std::string &method, const std::string &suffix, std::vector<ddb::HttpResponse> replies)
        {
            // A route registered again replaces the one before it
            routes.erase(std::remove_if(routes.begin(), routes.end(),
                                        [&](const Route &r) { return r.method == method && r.suffix == suffix; }),
                         routes.end());
            Route route{method, suffix, {}};
            route.replies.assign(replies.begin(), replies.end());
            routes.push_back(std::move(route));
        }
        void on(const std::string &method, const std::string &suffix, ddb::HttpResponse replyOnce)
        {
            on(method, suffix, std::vector<ddb::HttpResponse>{std::move(replyOnce)});
        }

        std::vector<Call> callsTo(const std::string &method, const std::string &suffix) const
        {
            std::vector<Call> found;
            for (const auto &c : calls)
                if (c.method == method && c.path.size() >= suffix.size() &&
                    c.path.compare(c.path.size() - suffix.size(), suffix.size(), suffix) == 0)
                    found.push_back(c);
            return found;
        }

        ddb::HttpResponse get(const ddb::HttpRequest &request) override { return answer("GET", request, nullptr); }
        ddb::HttpResponse post(const ddb::HttpRequest &request) override { return answer("POST", request, nullptr); }
        ddb::HttpResponse multipart(const ddb::HttpRequest &request) override
        {
            return answer("MULTIPART", request, nullptr);
        }
        ddb::HttpResponse download(const ddb::HttpRequest &request, std::ostream &sink) override
        {
            return answer(request.form.empty() ? "DOWNLOAD" : "DOWNLOAD-POST", request, &sink);
        }
    };

    // The deps of a RegistryClient with nothing real in them: the fake client, a credentials file
    // of the test, a sleeper that records the waits instead of making them, and a clock the
    // test moves
    struct Network
    {
        HttpClient http;
        ddb::AuthManager credentials;
        std::vector<std::chrono::milliseconds> sleeps;
        std::time_t now = 1'000'000;
        std::chrono::milliseconds retryClock{0};

        explicit Network(const std::filesystem::path &authFile) : credentials(authFile) {}

        ddb::RegistryDeps deps()
        {
            ddb::RegistryDeps d;
            d.http = &http;
            d.credentials = &credentials;
            d.sleep = [this](std::chrono::milliseconds delay) {
                sleeps.push_back(delay);
                retryClock += delay;
            };
            d.retryClock = [this]() { return retryClock; };
            d.now = [this]() { return now; };
            return d;
        }

        // The login answer of a Registry: a token that expires `lifetime` seconds from now
        void acceptLogin(const std::string &token = "tok-1", std::time_t lifetime = 3600)
        {
            http.on("POST", "/users/authenticate",
                    jsonReply({{"token", token}, {"expires", static_cast<long long>(now + lifetime)}}));
        }
    };
} // namespace fake

#endif // FAKE_HTTP_CLIENT_H
