/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "mzip.h"
#include <zip.h>
#include "mio.h"
#include "logger.h"
#include "exceptions.h"

#include <fstream>
#include <algorithm>
#include <system_error>

namespace ddb::zip
{

#define COPY_BUF_SIZE 4096
#define ERR_SIZE 256

    // Rejects ZIP entry names that would escape the extraction directory (Zip-Slip):
    // absolute paths, Windows drive specifiers, and parent-directory traversal. This
    // guards every caller of _extractAll (registry pulls and .3tz builds) against
    // malicious archives, as required by the 3D Tiles archive security considerations.
    static void ensureSafeZipEntry(const std::string &outdir, const char *entryName)
    {
        std::string name(entryName == nullptr ? "" : entryName);
        std::replace(name.begin(), name.end(), '\\', '/');

        if (!name.empty() && name[0] == '/')
            throw ZipException("Unsafe absolute path in archive entry: " + name);
        if (name.size() >= 2 && name[1] == ':')
            throw ZipException("Unsafe drive path in archive entry: " + name);

        // Reject any '..' path segment.
        size_t start = 0;
        while (true)
        {
            const size_t slash = name.find('/', start);
            const std::string seg =
                name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (seg == "..")
                throw ZipException("Unsafe path traversal in archive entry: " + name);
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }

        // Defense-in-depth: verify the resolved target stays inside outdir.
        std::error_code baseEc, targetEc;
        const fs::path base = fs::weakly_canonical(io::fromUtf8(outdir), baseEc);
        const fs::path target =
            fs::weakly_canonical(io::fromUtf8(outdir) / io::fromUtf8(name), targetEc);
        if (!baseEc && !targetEc)
        {
            std::error_code relEc;
            const fs::path rel = fs::relative(target, base, relEc);
            // A path that escapes base resolves to a relative path whose first
            // component is "..". Comparing fs::path components is portable across
            // the narrow (POSIX) / wide (Windows) native string types.
            if (relEc || rel.empty() || *rel.begin() == fs::path(".."))
                throw ZipException("Archive entry escapes extraction directory: " + name);
        }
    }

    static void extractArchive(zip_t *pZip, const std::string &outdir)
    {
        struct zip_file *pFile = nullptr;
        int bytesRead;
        char buf[COPY_BUF_SIZE];

        io::createDirectories(io::fromUtf8(outdir));

        zip_int64_t nEntries = zip_get_num_entries(pZip, 0);

        try
        {
            for (zip_int64_t eId = 0; eId < nEntries; eId++)
            {
                struct zip_stat fStat;

                if (zip_stat_index(pZip, eId, 0, &fStat))
                {
                    throw ZipException("error reading file at index " + std::to_string(eId));
                }

                // Name check
                if (!(fStat.valid & ZIP_STAT_NAME))
                {
                    LOGD << "zip: skipping entry at index " << eId << "with invalid name";
                    continue;
                }

                // Reject unsafe entry names (Zip-Slip) before touching the filesystem.
                ensureSafeZipEntry(outdir, fStat.name);

                // Directory
                fs::path entryPath = io::fromUtf8(outdir) / io::fromUtf8(fStat.name);

                if ((fStat.name[0] != '\0') && (fStat.name[strlen(fStat.name) - 1] == '/'))
                {
                    io::assureFolderExists(entryPath);
                    continue;
                }

                // File
                io::assureFolderExists(entryPath.parent_path());

                // Open write stream
                std::ofstream of(entryPath, std::ios::binary | std::ios::out | std::ios::trunc);
                if (!of.is_open())
                {
                    throw ZipException("Cannot open " + io::toUtf8(entryPath) + " for writing");
                }

                // Open file in archive
                if ((pFile = zip_fopen_index(pZip, eId, 0)) == nullptr)
                {
                    throw ZipException("Error extracting file: " + std::string(zip_strerror(pZip)));
                }

                // Extract
                do
                {
                    if ((bytesRead = zip_fread(pFile, buf, COPY_BUF_SIZE)) == -1)
                    {
                        throw ZipException("Error extracting file: " + std::string(zip_strerror(pZip)));
                    }

                    if (bytesRead > 0)
                    {
                        of.write(buf, bytesRead);
                    }
                } while (bytesRead > 0);

                zip_fclose(pFile);
                pFile = nullptr;
                of.close();
            }

            if (zip_close(pZip))
            {
                throw ZipException("Error closing archive: " + std::string(zip_strerror(pZip)));
            }
            pZip = nullptr;
        }
        catch (const ZipException &)
        {
            // Close files
            if (pFile != nullptr)
                zip_fclose(pFile);
            if (pZip != nullptr)
                zip_close(pZip);
            throw;
        }
    }

    void extractAll(const std::string &zipFile, const std::string &outdir)
    {
        int error;
        char errstr[ERR_SIZE];

        zip_t *pZip = zip_open(zipFile.c_str(), ZIP_RDONLY, &error);
        if (pZip == nullptr)
        {
            zip_error_to_str(errstr, ERR_SIZE, error, errno);
            throw ZipException("Cannot open zip file " + zipFile + " (" + std::string(errstr) + ")");
        }

        extractArchive(pZip, outdir);
    }

}
