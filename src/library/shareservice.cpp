/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareservice.h"

#include "shareclient.h"

#include <algorithm>
#include <chrono>

#include "exceptions.h"
#include "fs.h"
#include "logger.h"
#include "mio.h"
#include "registryutils.h"

namespace ddb
{

    namespace
    {

        // The files of `input` that exist (relative ones against `cwd`); directories are skipped:
        // only file entries can be uploaded
        std::vector<fs::path> existingFiles(const std::vector<std::string> &input, const std::string &cwd)
        {
            std::vector<fs::path> filePaths;
            for (const auto &s : input)
            {
                fs::path p = io::fromUtf8(s);
                // Resolve relative paths against cwd before existence check,
                // so we don't incorrectly drop valid files when cwd is set.
                fs::path resolvedPath = p;
                if (!cwd.empty() && p.is_relative())
                    resolvedPath = io::fromUtf8(cwd) / p;

                if (fs::exists(resolvedPath) && !fs::is_directory(resolvedPath))
                    filePaths.push_back(resolvedPath);
            }
            return filePaths;
        }

        // The folder the paths of the share are relative to: `cwd`, or what the files have in common
        io::Path shareRoot(const std::vector<std::string> &input, const std::string &cwd)
        {
            if (!cwd.empty())
                return io::Path(io::fromUtf8(cwd));

            std::vector<fs::path> paths = io::fromUtf8(input);

            if (paths.size() == 1)
                return io::Path(paths.front().parent_path());

            fs::path commonDir = io::commonDirPath(paths);
            if (commonDir.empty())
                throw InvalidArgsException(
                    "Cannot share files that don't have a common directory (are "
                    "you trying to share files from different drives?)");
            return io::Path(commonDir);
        }

        // init, one upload per file, commit: the share, on a client that is logged in
        std::string uploadShare(RegistryClient &reg, const std::string &tag, const std::string &password,
                                const std::vector<fs::path> &filePaths, io::Path wd,
                                const ShareCallback &cb)
        {
            ShareClient client(&reg);

            client.Init(tag, password);

            // TODO: multi threaded share/upload

            std::vector<ShareFileProgress *> files;
            ShareFileProgress sfp;
            files.push_back(&sfp);

            // Calculate total size
            size_t gTotalBytes = 0;
            size_t gTxBytes = 0;

            auto lastProgressUpdate = std::chrono::system_clock::now();
            auto t100ms = std::chrono::milliseconds(100);

            for (auto &fp : filePaths)
            {
                gTotalBytes += io::Path(fp).getSize();
            }

            for (auto &fp : filePaths)
            {
                auto p = io::Path(fp);
                auto fileSize = p.getSize();
                auto fileName = io::toUtf8(fp.filename());

                LOGD << "Current Path = " << fp;

                sfp.filename = fileName;
                sfp.totalBytes = fileSize;
                sfp.txBytes = 0;

                p = p.isAbsolute() && !wd.isParentOf(p.get()) ? p.withoutRoot() : p.relativeTo(wd.get());

                LOGD << "Uploading " << io::toUtf8(p.get());

                client.Upload(
                    p.generic(), fp,
                    [&cb, &files, &sfp, &fileSize, &gTotalBytes, &gTxBytes, &lastProgressUpdate, t100ms](
                        std::string &, size_t txBytes, size_t)
                    {
                        if (cb == nullptr)
                            return true;

                        // We cap the txBytes from CURL since it
                        // includes data transferred from the
                        // request

                        // CAUTION: this will require a lock if you
                        // use threads

                        gTxBytes -= sfp.txBytes;
                        sfp.txBytes = std::min(fileSize, txBytes);
                        gTxBytes += sfp.txBytes;

                        const auto now = std::chrono::system_clock::now();
                        if (lastProgressUpdate + t100ms < now)
                        {
                            lastProgressUpdate = now;
                            return cb(files, gTxBytes, gTotalBytes);
                        }

                        return true;
                    });
            }

            return client.Commit();
        }

    } // namespace

    ShareService::ShareService(const RegistryDeps &registryDeps) : deps(registryDeps) {}

    std::string ShareService::share(const std::vector<std::string> &input,
                                    const std::string &tag,
                                    const std::string &password,
                                    CredentialProvider &credentials,
                                    const std::string &cwd,
                                    const ShareCallback &cb,
                                    bool sslVerify)
    {
        if (input.empty())
            throw InvalidArgsException("No files to share");

        // Input is expected to be already expanded (e.g. via expandGlobPatterns).
        const std::vector<fs::path> filePaths = existingFiles(input, cwd);

        if (filePaths.empty())
            throw InvalidArgsException("No files to share");

        // Parse tag to find registry URL
        const TagComponents tc = RegistryUtils::parseTag(tag);

        // Before anything reaches the server: a share that cannot be laid out is not started
        io::Path wd = shareRoot(input, cwd);

        std::string resultUrl;
        withAuthenticatedRegistry(
            tc.registryUrl, sslVerify, credentials,
            [&](RegistryClient &reg) { resultUrl = uploadShare(reg, tc.tagWithoutUrl(), password, filePaths, wd, cb); },
            {}, deps);

        LOGD << "Result url " << resultUrl;

        return resultUrl;
    }

} // namespace ddb
