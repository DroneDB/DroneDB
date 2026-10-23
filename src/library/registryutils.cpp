/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "registryutils.h"
#include "stringutil.h"

#include "url.h"

#include "constants.h"
#include "exceptions.h"
#include "utils.h"
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

        if (t.empty() || useDefaultRegistry)
        {
            // Use default registry URL
            res.registryUrl =
                std::string(useInsecureRegistry ? "http://" : "https://") +
                DEFAULT_REGISTRY;
        }
        else
        {
            // TODO: should we validate the URL more thoroughly?
            const bool hasProto = t.starts_with("http://") || t.starts_with("https://");
            if (hasProto)
                res.registryUrl = t;
            else
                res.registryUrl =
                    (useInsecureRegistry ? "http://" : "https://") + t;
        }

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

        const homer6::Url url(res.registryUrl);

        // Get rid of path
        res.registryUrl = url.getScheme() + "://" + url.getHost();

        // Add port if needed
        if ((url.getScheme() == "http" && url.getPort() != 80) ||
            (url.getScheme() == "https" && url.getPort() != 443))
            res.registryUrl += ":" + std::to_string(url.getPort());

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

    std::string RegistryUtils::transportError(const cpr::Response &res, const std::string &url)
    {
        if (res.error.code == cpr::ErrorCode::OK)
            return "";

        const std::string reason = res.error.message.empty() ? "unknown error" : res.error.message;
        switch (res.error.code)
        {
        case cpr::ErrorCode::COULDNT_CONNECT:
        case cpr::ErrorCode::COULDNT_RESOLVE_HOST:
        case cpr::ErrorCode::COULDNT_RESOLVE_PROXY:
            return "could not connect to " + url + ": " + reason;
        default:
            return "request to " + url + " failed: " + reason;
        }
    }

    Registry RegistryUtils::createFromTag(const std::string &tag,
                                          bool useInsecureRegistry,
                                          bool sslVerify)
    {
        const auto tc = parseTag(tag, useInsecureRegistry);
        return Registry(tc.registryUrl, sslVerify);
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
