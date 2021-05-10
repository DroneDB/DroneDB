/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareservice.h"

#include <shareclient.h>

#include "dbops.h"
#include "fs.h"
#include "mio.h"
#include "registry.h"
#include "registryutils.h"
#include "userprofile.h"
#include "utils.h"

namespace ddb {

ShareService::ShareService() {}

std::string ShareService::share(const std::vector<std::string> &input,
                                const std::string &tag,
                                const std::string &password, bool recursive,
                                const std::string &cwd,
                                const ShareCallback &cb) {
    if (input.empty()) throw InvalidArgsException("No files to share");

    std::vector<fs::path> filePaths =
        getPathList(input, false, recursive ? 0 : 1);

    // Parse tag to find registry URL
    TagComponents tc = RegistryUtils::parseTag(tag);
    AuthCredentials ac =
        UserProfile::get()->getAuthManager()->loadCredentials(tc.registryUrl);
    if (ac.empty()) throw AuthException("No authentication credentials stored");

    Registry reg(tc.registryUrl);
    reg.login(ac.username, ac.password);  // Will throw error on login failed

    ShareClient client(&reg);

    client.Init(tc.tagWithoutUrl(), password);

    // TODO: multi threaded share/upload

    // Calculate cwd from paths or use the one provided?
    io::Path wd;
    if (cwd.empty()) {
        std::vector<fs::path> paths(input.begin(), input.end());

        if (paths.size() == 1){
            wd = io::Path(paths.front().parent_path());
        }else{
            fs::path commonDir = io::commonDirPath(paths);
            if (commonDir.empty())
                throw InvalidArgsException(
                    "Cannot share files that don't have a common directory (are "
                    "you trying to share files from different drives?)");
            wd = io::Path(commonDir);
        }
    } else {
        wd = io::Path(fs::path(cwd));
    }

    std::vector<ShareFileProgress *> files;
    ShareFileProgress sfp;
    files.push_back(&sfp);

    // Calculate total size
    size_t gTotalBytes = 0;
    size_t gTxBytes = 0;

    auto lastProgressUpdate = std::chrono::system_clock::now();
    auto t100ms = std::chrono::milliseconds(100);

    for (auto &fp : filePaths) {
        gTotalBytes += io::Path(fp).getSize();
    }

    for (auto &fp : filePaths) {
        auto p = io::Path(fp);
        auto fileSize = p.getSize();
        auto fileName = fp.filename().string();

        LOGD << "Current Path = " << fp;

        sfp.filename = fileName;
        sfp.totalBytes = fileSize;
        sfp.txBytes = 0;

        p = p.isAbsolute() && !wd.isParentOf(p.get()) ? 
                p.withoutRoot() : p.relativeTo(wd.get());
                
        LOGD << "Uploading " << p.string();

        client.Upload(
            p.generic(), fp,
            [&cb, &files, &sfp, &fileSize, &gTotalBytes, &gTxBytes, &lastProgressUpdate, t100ms](
                std::string &fileName, size_t txBytes, size_t totalBytes) {
                if (cb == nullptr) return true;

                // We cap the txBytes from CURL since it
                // includes data transferred from the
                // request

                // CAUTION: this will require a lock if you
                // use threads

                gTxBytes -= sfp.txBytes;
                sfp.txBytes = std::min(fileSize, txBytes);
                gTxBytes += sfp.txBytes;

                const auto now = std::chrono::system_clock::now();
                if (lastProgressUpdate + t100ms < now){
                    lastProgressUpdate = now;
                    return cb(files, gTxBytes, gTotalBytes);
                }

                return true;
            });
        
    }

    auto resultUrl = client.Commit();

    LOGD << "Result url " << resultUrl;

    return resultUrl;
}

}  // namespace ddb
