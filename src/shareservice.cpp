/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "shareservice.h"
#include "registry.h"
#include "registryutils.h"
#include "fs.h"
#include "mio.h"
#include "dbops.h"
#include "utils.h"
#include "userprofile.h"

namespace ddb{

ShareService::ShareService(){

}

std::string ShareService::share(const std::vector<std::string> &input, const std::string &tag,
                                const std::string &password, bool recursive, const std::string &cwd,
                                const ShareCallback &cb){
    if (input.size() == 0) throw InvalidArgsException("No files to share");

    std::vector<fs::path> filePaths = getPathList(input, false, recursive ? 0 : 1);

    // Parse tag to find registry URL
    TagComponents tc = RegistryUtils::parseTag(tag);
    AuthCredentials ac = UserProfile::get()->getAuthManager()->loadCredentials(tc.registryUrl);
    if (ac.empty()) throw AuthException("No authentication credentials stored");

    Registry reg(tc.registryUrl);
    std::string authToken = reg.login(ac.username, ac.password); // Will throw error on login failed

    // Call init
    net::Response res = net::POST(reg.getUrl("/share/init"))
            .formData({"tag", tc.tagWithoutUrl(), "password", password})
            .authToken(authToken)
            .send();
    if (res.status() != 200) handleError(res);

    json j = res.getJSON();
    if (!j.contains("token")) handleError(res);
    std::string batchToken = j["token"];

    // TODO: multithreaded share/upload

    // Calculate cwd from paths or use the one provided?
    io::Path wd;
    if (cwd.empty()){
        std::vector<fs::path> paths(input.begin(), input.end());
        fs::path commonDir = io::commonDirPath(paths);
        if (commonDir.empty()) throw InvalidArgsException("Cannot share files that don't have a common directory (are you trying to share files from different drives?)");
        wd = io::Path(commonDir);
    }else{
        wd = io::Path(fs::path(cwd));
    }

    std::vector<ShareFileProgress *> files;
    ShareFileProgress sfp;
    files.push_back(&sfp);

    // Calculate total size
    size_t gTotalBytes = 0;
    size_t gTxBytes = 0;

    for (auto &fp : filePaths){
        gTotalBytes += io::Path(fp).getSize();
    }

    // Upload
    const int MAX_RETRIES = 10;

    for (auto &fp : filePaths){
        io::Path p = io::Path(fp);

        std::string sha256 = Hash::fileSHA256(fp.string());
        std::string filename = fp.filename().string();
        size_t filesize = p.getSize();

        sfp.filename = filename;
        sfp.totalBytes = filesize;
        sfp.txBytes = 0;

        if (p.isAbsolute() && !wd.isParentOf(p.get())){
            p = p.withoutRoot();
        }else{
            p = p.relativeTo(wd.get());
        }

        LOGD << "Uploading " << p.string();

        int retryNum = 0;

        while(true){
            try{
                net::Response res = net::POST(reg.getUrl("/share/upload/" + batchToken))
                        .multiPartFormData({"file", fp.string()},
                                           {"path", p.generic()})
                        .authToken(authToken)
                        .progressCb([&cb, &files, &sfp, &filesize, &gTotalBytes, &gTxBytes](size_t txBytes, size_t totalBytes){
                            if (cb == nullptr) return true;
                            else{
                                // We cap the txBytes from CURL since it
                                // includes data transferred from the request

                                // CAUTION: this will require a lock if you use threads

                                gTxBytes -= sfp.txBytes;
                                sfp.txBytes = std::min(filesize, txBytes);
                                gTxBytes += sfp.txBytes;
                                return cb(files, gTxBytes, gTotalBytes);
                            }
                        })
                        //.maximumUploadSpeed(1024*1024)
                        .send();

                // Token expired?
                if (res.status() == 401){
                    LOGD << "Token expired";
                    authToken = reg.login(ac.username, ac.password);
                    throw NetException("Unauthorized");
                }

                if (res.status() != 200) handleError(res);

                json j = res.getJSON();
                if (!j.contains("hash")) handleError(res);
                if (sha256 != j["hash"]) throw NetException(filename + " file got corrupted during upload (hash mismatch, expected: " +
                                                                sha256 + ", got: " + j["hash"].get<std::string>() + ". Try again.");
                break; // Done
            }catch(const NetException &e){
                if (++retryNum >= MAX_RETRIES) throw e;
                else{
                    LOGD << e.what() << ", retrying upload of " << filename << " (attempt " << retryNum << ")";
                    utils::sleep(1000 * retryNum);
                }
            }
        }
    }

    // Commit
    int retryNum = 0;
    while (true){
        try{
            res = net::POST(reg.getUrl("/share/commit/" + batchToken))
                    .authToken(authToken)
                    .send();

            if (res.status() == 401){
                LOGD << "Token expired";
                authToken = reg.login(ac.username, ac.password);
                throw NetException("Unauthorized");
            }

            if (res.status() != 200) handleError(res);

            j = res.getJSON();
            if (!j.contains("url")) handleError(res);

            return reg.getUrl(j["url"]); // Done
        }catch(const NetException &e){
            if (++retryNum >= MAX_RETRIES) throw e;
            else{
                LOGD << e.what() << ", retrying commit (attempt " << retryNum << ")";
                utils::sleep(1000 * retryNum);
            }
        }
    }
}

void ShareService::handleError(net::Response &res){
    if (res.hasData()){
        LOGD << "Request error: " << res.getText();

        auto j = res.getJSON();
        if (j.contains("error")) throw RegistryException("Error response from registry: " + j["error"].get<std::string>());
        else throw RegistryException("Invalid response from registry: " + res.getText());
    }else{
        LOGD << "Request error: " << res.status();

        throw RegistryException("Invalid response from registry. Returned status: " + std::to_string(res.status()));
    }
}

}
