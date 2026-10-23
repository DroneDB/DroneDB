/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/share.h"

#include <cstdlib>

#include "constants.h"
#include "dbops.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "include/progressbar.h"
#include "include/credentials.h"
#include "registry_client.h"
#include "registryutils.h"
#include "shareservice.h"

namespace cmd
{

    namespace
    {
        // Returns env var value or empty string if unset.
        std::string envOrEmpty(const char *name)
        {
            const char *v = std::getenv(name);
            return v ? std::string(v) : std::string();
        }
    }

    void Share::setOptions(cxxopts::Options &opts)
    {
        opts.positional_help("[args]")
            .custom_help("share *.JPG")
            .add_options()
            ("i,input", "Files, directories and glob patterns to share (e.g. *.JPG, **/*.tif)", cxxopts::value<std::vector<std::string>>())
            ("t,tag", "Tag to use (organization/dataset or server[:port]/organization/dataset)", cxxopts::value<std::string>()->default_value(DEFAULT_REGISTRY "//"))
            ("p,password", "Optional password to protect dataset", cxxopts::value<std::string>()->default_value(""))
            ("s,server", "Registry server to share dataset with (alias of: -t <server>//)", cxxopts::value<std::string>())
            ("q,quiet", "Do not display progress", cxxopts::value<bool>());
        cmd::addInsecureOption(opts);
        opts.add_options()
            ("non-interactive", "Do not prompt for credentials. Uses DDB_USERNAME and DDB_PASSWORD env vars if available.", cxxopts::value<bool>());

        opts.parse_positional({"input"});
    }

    std::string Share::description()
    {
        return "Share files and folders to a registry";
    }

    int Share::run(cxxopts::ParseResult &opts)
    {
        if (!opts.count("input"))
        {
            usageError("missing required argument 'input'");
        }

        auto rawInput = opts["input"].as<std::vector<std::string>>();
        auto input = ddb::expandGlobPatterns(rawInput);

        auto tag = opts["tag"].as<std::string>();
        if (opts["server"].count() && !opts["tag"].count())
        {
            tag = opts["server"].as<std::string>() + "//";
        }
        auto password = opts["password"].as<std::string>();
        auto quiet = opts["quiet"].count() > 0;
        auto sslVerify = opts["insecure"].count() == 0;
        auto nonInteractive = opts["non-interactive"].count() > 0;
        auto cwd = ddb::io::toUtf8(ddb::io::getCwd());

        ProgressBar pb;
        ddb::ShareCallback showProgress =
            [&pb](const std::vector<ddb::ShareFileProgress *> &files,
                  size_t, size_t)
        {
            if (!files.empty())
            {

                auto *f = files[0];
                const float progress =
                    f->txBytes > 0
                        ? static_cast<float>(f->txBytes) /
                              static_cast<float>(f->totalBytes) * 100.0f
                        : 0.0f;

                // TODO: support for parallel progress updates
                pb.update(f->filename, progress);
            }
            return true;
        };
        if (quiet)
            showProgress = nullptr;

        // With --non-interactive nobody is asked: the credentials come from the saved ones and the
        // environment only
        InteractiveCredentials interactive;
        RefusedCredentials refused("Authentication failed. Set DDB_USERNAME and "
                                   "DDB_PASSWORD env vars or run `ddb login` first.");
        ddb::CredentialProvider &credentials = nonInteractive ? static_cast<ddb::CredentialProvider &>(refused)
                                                              : static_cast<ddb::CredentialProvider &>(interactive);

        // Try env-var credentials before the share attempt so that the very
        // first request has them available (a login saves the credentials, which is
        // where the share looks for them).
        const auto envUser = envOrEmpty("DDB_USERNAME");
        const auto envPass = envOrEmpty("DDB_PASSWORD");
        if (!envUser.empty() && !envPass.empty())
        {
            try
            {
                ddb::RegistryClient reg = ddb::RegistryUtils::createFromTag(tag, false, sslVerify);
                reg.login(envUser, envPass);
            }
            catch (const ddb::AppException &e)
            {
                LOGD << "Env-var login failed: " << e.what();
            }
        }

        try
        {
            ddb::ShareService ss;
            const std::string url =
                ss.share(input, tag, password, credentials, cwd, showProgress, sslVerify);
            if (!quiet)
                pb.done();
            std::cout << url << std::endl;
        }
        catch (const ddb::AppException &e)
        {
            if (!quiet)
                pb.done();
            throw;
        }
        return EXIT_SUCCESS;
    }

} // namespace cmd
