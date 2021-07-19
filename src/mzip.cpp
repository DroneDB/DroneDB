/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <zip.h>

#include "mzip.h"
#include "mio.h"
#include "logger.h"
#include "exceptions.h"

namespace ddb::zip {

#define COPY_BUF_SIZE 4096
#define ERR_SIZE 256

void extractAll(const std::string &zipFile, const std::string &outdir, std::ostream *progressOut){
    io::createDirectories(outdir);

    struct zip_file* pFile = nullptr;
    int bytesRead;
    char buf[COPY_BUF_SIZE];

    int error;
    char errstr[ERR_SIZE];

    zip_t* pZip = zip_open(zipFile.c_str(), 0, &error);
    if (pZip == nullptr) {
        zip_error_to_str(errstr, ERR_SIZE, error, errno);
        throw ZipException("Cannot open zip file " + zipFile + " (" + std::string(errstr) + ")");
    }

    zip_int64_t nEntries = zip_get_num_entries(pZip, 0);

    try{
        for (zip_int64_t eId = 0; eId < nEntries; eId++) {
            struct zip_stat fStat;

            if(zip_stat_index(pZip, eId, 0, &fStat)) {
              throw ZipException("error reading file at index " + std::to_string(eId));
            }

            // Name check
            if(!(fStat.valid & ZIP_STAT_NAME)) {
              LOGD << "zip: skipping entry at index " << eId << "with invalid name";
              continue;
            }

            // Directory
            fs::path entryPath = fs::path(outdir) / fs::path(fStat.name);

            if((fStat.name[0] != '\0') && (fStat.name[strlen(fStat.name)-1] == '/')) {
              io::assureFolderExists(entryPath);
              continue;
            }

            // File
            io::assureFolderExists(entryPath.parent_path());

            // Open write stream
            std::ofstream of(entryPath.string(), std::ios::binary | std::ios::out | std::ios::trunc);
            if (!of.is_open()){
                throw ZipException("Cannot open " + entryPath.string() + " for writing");
            }

            // Open file in archive
            if((pFile = zip_fopen_index(pZip, eId, 0)) == nullptr) {
              throw ZipException("Error extracting file: " + std::string(zip_strerror(pZip)));
            }

            // Extract
            do {
                if((bytesRead = zip_fread(pFile, buf, COPY_BUF_SIZE)) == -1) {
                    throw ZipException("Error extracting file: " + std::string(zip_strerror(pZip)));
                }

                if (bytesRead > 0){
                    of.write(buf, bytesRead);
                }
            } while(bytesRead > 0);

            zip_fclose(pFile);
            pFile = nullptr;
            of.close();

            if (progressOut != nullptr){
                (*progressOut) << "Extracted (" << (eId + 1) << "/" << nEntries << ")\t\t\r";
                progressOut->flush();
            }
        }

        if(zip_close(pZip)) {
            throw ZipException("Error closing archive: " + std::string(zip_strerror(pZip)));
        }
        pZip = nullptr;
    }catch(const ZipException &e){
        // Close files
        if (pFile != nullptr) zip_fclose(pFile);
        if (pZip != nullptr) zip_close(pZip);
        throw;
    }

    if (progressOut != nullptr) (*progressOut) << std::endl;
}

void zipFolder(const std::string &folder, const std::string &zipFile, const std::vector<std::string> &excludes){
    int error;
    char errstr[ERR_SIZE];

    zip_t* pZip = zip_open(zipFile.c_str(), ZIP_CREATE | ZIP_EXCL | ZIP_TRUNCATE, &error);
    if (pZip == nullptr) {
        zip_error_to_str(errstr, ERR_SIZE, error, errno);
        throw ZipException("Cannot open zip file " + zipFile + " (" + std::string(errstr) + ")");
    }

    zip_source_t *source = nullptr;

    try{
        for (auto i = fs::recursive_directory_iterator(folder);
             i != fs::recursive_directory_iterator(); ++i) {

            const auto relPath = io::Path(i->path()).relativeTo(folder);

            bool exclude = false;

            for (const auto &excl : excludes) {
                exclude = false;

                // If it's a folder we exclude this path and all the descendants
                if (excl[excl.length() - 1] == '/') {
                    const auto folderName = excl.substr(0, excl.length() - 1);
                    if (relPath.generic().find(folderName) == 0) {
                        exclude = true;
                        i.disable_recursion_pending();
                        break;
                    }
                } else {
                    if (relPath.generic() == excl) {
                        exclude = true;
                        break;
                    }
                }
            }
            if (!exclude) {
                LOGD << "Adding: '" << relPath.generic() << "'";
                if (fs::is_directory(i->path())){
                    if (zip_dir_add(pZip, relPath.generic().c_str(), ZIP_FL_ENC_UTF_8) < 0){
                        throw ZipException("Cannot add directory to zip: " +  std::string(zip_strerror(pZip)));
                    }
                }else{
                    source = zip_source_file(pZip, i->path().string().c_str(), 0, 0);
                    if (source == nullptr){
                        throw ZipException("Failed to add file to zip: " + std::string(zip_strerror(pZip)));
                    }

                    if (zip_file_add(pZip, relPath.generic().c_str(), source, ZIP_FL_ENC_UTF_8) < 0){
                        throw ZipException("Failed to add file to zip: " + std::string(zip_strerror(pZip)));
                    }

                    source = nullptr;
                }
            }
        }

        if(zip_close(pZip)) {
            throw ZipException("Error closing archive: " + std::string(zip_strerror(pZip)));
        }
        pZip = nullptr;

    }catch(const ZipException &e){
        if (pZip != nullptr) zip_close(pZip);
        if (source != nullptr) zip_source_free(source);
        throw;
    }
}


}

