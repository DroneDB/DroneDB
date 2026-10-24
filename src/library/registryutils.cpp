/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "registryutils.h"
#include "json.h"
#include "stringutil.h"

#include "../../vendor/url/Url.h"

#include "constants.h"
#include "exceptions.h"
#include <regex>

namespace ddb
{
    // Regex for validating organization and dataset names
    // Must start with lowercase letter or digit, then lowercase letters, digits, underscores or dashes
    // Total length: 2-129 characters
    static const std::regex tagComponentRegex("^[a-z0-9][a-z0-9_-]{1,128}$");

    static void validateTagComponent(const std::string &component, const std::string &componentType, const std::string &originalTag) {
        if (component.empty()) {
            throw InvalidArgsException("Invalid tag: " + originalTag + " - " + componentType + " is empty");
        }
        if (!std::regex_match(component, tagComponentRegex)) {
            throw InvalidArgsException("Invalid tag: " + originalTag + " - " + componentType + " '" + component +
                                       "' must start with a lowercase letter or digit, contain only lowercase letters, digits, underscores or dashes, and be 2-129 characters long");
        }
    }

    std::string RegistryUtils::normalizeUrl(const std::string &url, bool useInsecureRegistry, bool keepPath)
    {
        auto text = url;
        str::trim(text);
        if (text.empty())
            text = DEFAULT_REGISTRY;

        const auto invalid = [&url](const std::string &why) {
            return InvalidArgsException("Invalid registry URL '" + url + "': " + why);
        };

        std::string scheme = useInsecureRegistry ? "http" : "https";
        const auto separator = text.find("://");
        if (separator != std::string::npos)
        {
            scheme = text.substr(0, separator);
            str::toLower(scheme);
            text = text.substr(separator + 3);
        }

        if (scheme != "http" && scheme != "https")
            throw invalid("only http and https are supported");

        homer6::Url parsed;
        try
        {
            parsed.fromString(scheme + "://" + text);
        }
        catch (const std::exception &e)
        {
            throw invalid(e.what());
        }

        if (parsed.getHost().empty())
            throw invalid("it has no host");

        std::string result = parsed.getScheme() + "://" + parsed.getHost();

        if ((parsed.getScheme() == "http" && parsed.getPort() != 80) ||
            (parsed.getScheme() == "https" && parsed.getPort() != 443))
            result += ":" + std::to_string(parsed.getPort());

        if (keepPath)
        {
            auto path = parsed.getPath();
            while (!path.empty() && path.back() == '/')
                path.pop_back();
            result += path;
        }

        return result;
    }

    TagComponents RegistryUtils::parseTag(const std::string &tag,
                                          bool useInsecureRegistry)
    {
        auto t = tag;
        str::trim(t);
        str::toLower(t);

        // Remove trailing slashes
        while (!t.empty() && t.back() == '/')
            t.pop_back();

        auto pos = t.rfind('/');
        if (pos == std::string::npos)
            throw InvalidArgsException("Invalid tag: " + tag +
                                       " must be in organization/dataset format");

        TagComponents res;
        res.dataset = t.substr(pos + 1);
        t = t.substr(0, pos);

        pos = t.rfind('/');
        auto useDefaultRegistry = false;

        if (pos == std::string::npos)
        {
            res.organization = t;
            useDefaultRegistry = true;
        }
        else
        {
            res.organization = t.substr(pos + 1);
        }
        t = t.substr(0, t.length() - res.organization.length() - 1);

        // The server part of the tag, or the default registry when it has none
        const std::string server = (t.empty() || useDefaultRegistry) ? std::string() : t;

        // Check that we haven't parsed the server part into the
        // organization name
        if (res.organization.starts_with("http://") ||
            res.organization.starts_with("https://"))
        {
            throw InvalidArgsException("Invalid tag: " + tag +
                                       " missing dataset name");
        }

        // Validate organization and dataset components
        validateTagComponent(res.organization, "organization", tag);
        validateTagComponent(res.dataset, "dataset", tag);

        // Without the path, and only with a scheme the client speaks
        res.registryUrl = normalizeUrl(server, useInsecureRegistry);

        return res;
    }

    std::string RegistryUtils::errorMessage(const std::string &body)
    {
        const json j = json::parse(body, nullptr, false);
        if (!j.is_object())
            return "";
        for (const char *field : {"detail", "error", "message", "title"})
        {
            const auto it = j.find(field);
            if (it != j.end() && it->is_string() && !it->get<std::string>().empty())
                return it->get<std::string>();
        }
        return "";
    }

    bool RegistryUtils::noRetry(const std::string &body)
    {
        const json j = json::parse(body, nullptr, false);
        if (!j.is_object())
            return false;
        const auto it = j.find("noRetry");
        return it != j.end() && it->is_boolean() && it->get<bool>();
    }

    std::string RegistryUtils::transportError(const HttpResponse &res, const std::string &url)
    {
        if (!res.hasTransportError())
            return "";

        const std::string reason = res.transportMessage.empty() ? "unknown error" : res.transportMessage;
        if (res.transport == HttpTransportError::CouldNotConnect)
            return "could not connect to " + url + ": " + reason;
        return "request to " + url + " failed: " + reason;
    }

    RegistryClient RegistryUtils::createFromTag(const std::string &tag,
                                                bool useInsecureRegistry,
                                                bool sslVerify,
                                                const RegistryDeps &deps)
    {
        const auto tc = parseTag(tag, useInsecureRegistry);
        return RegistryClient(tc.registryUrl, sslVerify, deps);
    }

    std::string TagComponents::tagWithoutUrl() const
    {
        if (!organization.empty() && !dataset.empty())
        {
            return organization + "/" + dataset;
        }
        return "";
    }

    // Tag that always include explicit protocol/server information
    // e.g. https://server/org/ds
    std::string TagComponents::fullTag() const
    {
        std::string tmp;

        if (!registryUrl.empty())
            tmp += registryUrl + "/";

        if (organization.empty() && dataset.empty())
            return "";

        return tmp + organization + "/" + dataset;
    }

    std::string TagComponents::tag() const
    {
        std::string tmp;

        if (!registryUrl.empty())
            tmp += registryUrl + "/";

        // Implicit
        if (tmp == "https://" DEFAULT_REGISTRY "/")
        {
            tmp = "";
        }

        if (organization.empty() && dataset.empty())
            return "";

        return tmp + organization + "/" + dataset;
    }

} // namespace ddb
