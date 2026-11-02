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
#include <cctype>
#include <cstring>
#include <memory>

namespace ddb::zip
{

#define COPY_BUF_SIZE 4096
#define ERR_SIZE 256

    // A segment that Windows cannot create or silently turns into something else: a trailing dot
    // or space is dropped (so "a." and "a" are one file), ':' selects an NTFS alternate data
    // stream or a drive, and the device names (also with an extension, "NUL.txt") open the device.
    // Refused on every platform when the options ask for it, so an archive extracts to the same
    // tree everywhere.
    static void ensureWindowsSafeSegment(const std::string &seg, const std::string &name)
    {
        if (seg.empty() || seg == ".")
            return;

        if (seg.find(':') != std::string::npos)
            throw ZipException("Unsafe ':' in archive entry: " + name);
        if (seg.back() == '.' || seg.back() == ' ')
            throw ZipException("Unsafe trailing dot or space in archive entry: " + name);

        // The device name is what precedes the first dot, without the spaces before it
        std::string stem = seg.substr(0, seg.find('.'));
        while (!stem.empty() && stem.back() == ' ')
            stem.pop_back();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        const bool numberedDevice = stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
                                    (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0);
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || numberedDevice)
            throw ZipException("Unsafe device name in archive entry: " + name);
    }

    // Rejects ZIP entry names that would escape the extraction directory (Zip-Slip):
    // absolute paths, Windows drive specifiers, and parent-directory traversal, and, when
    // windowsSafe, names Windows cannot create (see ensureWindowsSafeSegment). This guards every
    // caller of extractAll (registry pulls and .3tz builds) against malicious archives, as required by
    // the 3D Tiles archive security considerations.
    static void ensureSafeZipEntry(const std::string &outdir, const char *entryName, bool windowsSafe)
    {
        std::string name(entryName == nullptr ? "" : entryName);
        std::replace(name.begin(), name.end(), '\\', '/');

        if (!name.empty() && name[0] == '/')
            throw ZipException("Unsafe absolute path in archive entry: " + name);
        if (name.size() >= 2 && name[1] == ':')
            throw ZipException("Unsafe drive path in archive entry: " + name);

        // Reject any '..' path segment, and every segment Windows cannot create.
        size_t start = 0;
        while (true)
        {
            const size_t slash = name.find('/', start);
            const std::string seg =
                name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (seg == "..")
                throw ZipException("Unsafe path traversal in archive entry: " + name);
            if (windowsSafe)
                ensureWindowsSafeSegment(seg, name);
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

    namespace
    {
        struct ZipCloser
        {
            void operator()(zip_t *z) const { zip_close(z); }
        };
        struct ZipFileCloser
        {
            void operator()(zip_file_t *f) const { zip_fclose(f); }
        };
        using ZipPtr = std::unique_ptr<zip_t, ZipCloser>;
        using ZipFilePtr = std::unique_ptr<zip_file_t, ZipFileCloser>;

        // The sum a hostile archive controls: it saturates instead of wrapping to a small number.
        uint64_t saturatingAdd(uint64_t a, uint64_t b)
        {
            return b > UINT64_MAX - a ? UINT64_MAX : a + b;
        }
    }

    // Reads the central directory once, before the output folder exists: checks every name and
    // the limits, and returns the declared uncompressed total. Nothing has been written when it throws.
    static uint64_t checkArchive(zip_t *pZip, const std::string &outdir, uint64_t archiveSize,
                                 const ExtractOptions &options)
    {
        const zip_int64_t nEntries = zip_get_num_entries(pZip, 0);
        if (nEntries < 0)
            throw ZipException("error reading the entries of the archive");
        if (static_cast<uint64_t>(nEntries) > options.maxEntries)
            throw ZipException("Archive has " + std::to_string(nEntries) + " entries, more than the " +
                               std::to_string(options.maxEntries) + " allowed");

        uint64_t declaredTotal = 0;
        for (zip_int64_t eId = 0; eId < nEntries; eId++)
        {
            struct zip_stat fStat;

            if (zip_stat_index(pZip, eId, 0, &fStat))
                throw ZipException("error reading file at index " + std::to_string(eId));

            if (!(fStat.valid & ZIP_STAT_NAME))
                continue;

            ensureSafeZipEntry(outdir, fStat.name, options.refuseWindowsUnsafeNames);
            if (options.acceptEntry && !options.acceptEntry(fStat.name))
                throw ZipException(std::string("Archive entry not allowed here: ") + fStat.name);

            if (!(fStat.valid & ZIP_STAT_SIZE))
                throw ZipException(std::string("Archive entry without a size: ") + fStat.name);
            declaredTotal = saturatingAdd(declaredTotal, fStat.size);
        }

        const bool ratioChecked = options.maxExpansionRatio != 0 &&
                                  archiveSize <= UINT64_MAX / options.maxExpansionRatio;
        if (ratioChecked && declaredTotal > archiveSize * options.maxExpansionRatio)
            throw ZipException("Archive expands to " + std::to_string(declaredTotal) + " bytes from " +
                               std::to_string(archiveSize) + " bytes, more than " +
                               std::to_string(options.maxExpansionRatio) + " times its size");

        return declaredTotal;
    }

    static void extractArchive(zip_t *pZip, const std::string &outdir, uint64_t archiveSize,
                               const ExtractOptions &options)
    {
        // Closed on every path out, also when a limit or the disk check throws
        ZipPtr guard(pZip);
        char buf[COPY_BUF_SIZE];

        const uint64_t declaredTotal = checkArchive(pZip, outdir, archiveSize, options);
        if (options.beforeWrite)
            options.beforeWrite(declaredTotal);

        io::createDirectories(io::fromUtf8(outdir));

        const zip_int64_t nEntries = zip_get_num_entries(pZip, 0);

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
            ZipFilePtr pFile(zip_fopen_index(pZip, eId, 0));
            if (pFile == nullptr)
            {
                throw ZipException("Error extracting file: " + std::string(zip_strerror(pZip)));
            }

            // Extract. The declared size was checked against the limits, so a member that
            // delivers more than it declared is cut off here instead of filling the disk.
            uint64_t written = 0;
            zip_int64_t bytesRead;
            while ((bytesRead = zip_fread(pFile.get(), buf, COPY_BUF_SIZE)) > 0)
            {
                written += static_cast<uint64_t>(bytesRead);
                if (written > fStat.size)
                {
                    throw ZipException(std::string("Archive entry ") + fStat.name +
                                       " is larger than the " + std::to_string(fStat.size) +
                                       " bytes it declares");
                }
                of.write(buf, bytesRead);
            }

            if (bytesRead == -1)
            {
                throw ZipException("Error extracting file: " + std::string(zip_strerror(pZip)));
            }

            pFile.reset();
            of.close();
        }

        if (zip_close(guard.release()))
        {
            throw ZipException("Error closing archive: " + std::string(zip_strerror(pZip)));
        }
    }

    void extractAll(const std::string &zipFile, const std::string &outdir, const ExtractOptions &options)
    {
        int error;
        char errstr[ERR_SIZE];

        zip_t *pZip = zip_open(zipFile.c_str(), ZIP_RDONLY, &error);
        if (pZip == nullptr)
        {
            zip_error_to_str(errstr, ERR_SIZE, error, errno);
            throw ZipException("Cannot open zip file " + zipFile + " (" + std::string(errstr) + ")");
        }

        std::error_code ec;
        const uintmax_t archiveSize = fs::file_size(io::fromUtf8(zipFile), ec);
        if (ec)
        {
            zip_close(pZip);
            throw ZipException("Cannot read the size of zip file " + zipFile + " (" + ec.message() + ")");
        }

        extractArchive(pZip, outdir, archiveSize, options);
    }

}
