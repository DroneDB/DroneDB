/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "../../vendor/base64/base64.h"
#include "authmanager.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"

namespace ddb
{

    void AuthManager::ReadFromDisk()
    {
        if (fs::exists(authFile))
        {
            std::ifstream fin(authFile);
            try
            {
                LOGD << "Read " << io::toUtf8(authFile);
                fin >> auth;
            }
            catch (const json::exception &e)
            {
                LOGD << "Error reading " << io::toUtf8(authFile) << ": " << e.what();
            }
        }
    }

    // The file holds passwords (base64 is an encoding, not protection): owner only (0600), and
    // replaced atomically so a crash or a full disk never leaves a half-written credentials file
    // that the next run would read as empty and overwrite. Throws FSException.
    void AuthManager::WriteToDisk()
    {
        io::writeFileAtomic(authFile, auth.dump(), "credentials file", true);
        LOGD << "Wrote " << io::toUtf8(authFile);
    }

    AuthManager::AuthManager(const fs::path &authFile) : authFile(authFile)
    {
        ReadFromDisk();

        // First time
        if (!auth.contains("auths"))
        {
            LOGD << "Initializing " << authFile;
            auth["auths"] = {};
            try
            {
                WriteToDisk();
            }
            catch (const FSException &e)
            {
                // Nothing to lose yet: a read-only profile folder only fails the saves that follow
                LOGW << "Cannot create " << io::toUtf8(authFile) << ": " << e.what();
            }
        }
    }

    void AuthManager::saveCredentials(const std::string &url, const AuthCredentials &creds)
    {
        LOGD << "Saving credentials for " << creds.username;

        std::stringstream ss;
        ss << creds.username << ":" << creds.password;
        auth["auths"][url] = {
            {"auth", Base64::encode(ss.str())}};

        WriteToDisk();
    }

    AuthCredentials AuthManager::loadCredentials(const std::string &url)
    {
        AuthCredentials ac;

        if (auth["auths"].contains(url))
        {
            const std::string userpwd = Base64::decode(auth["auths"][url]["auth"]);
            // "user:password": a user name has no colon (HTTP Basic, RFC 7617; Registry's names are
            // letters and digits) while a password may have any: split at the first one
            const size_t colonpos = userpwd.find(':');
            if (colonpos != std::string::npos && colonpos > 0)
            {
                LOGD << "Found username and password for " << url;
                ac.username = userpwd.substr(0, colonpos);
                ac.password = userpwd.substr(colonpos + 1, userpwd.length() - colonpos - 1);
            }
        }

        return ac;
    }

    bool AuthManager::deleteCredentials(const std::string &url)
    {
        if (auth["auths"].contains(url))
        {
            LOGD << "Deleting credentials for " << url;
            auth["auths"].erase(url);
            WriteToDisk();
            return true;
        }

        return false;
    }

    std::vector<std::string> AuthManager::getAuthenticatedRegistryUrls()
    {
        std::vector<std::string> result;
        for (auto it = auth["auths"].begin(); it != auth["auths"].end(); it++)
        {
            result.push_back(it.key());
        }
        return result;
    }

}
