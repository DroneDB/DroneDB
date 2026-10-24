/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef FILE_SIZE_LIMIT_H
#define FILE_SIZE_LIMIT_H

// A write that fails, for real: while a FileSizeLimit is alive the process may not write a file
// larger than the limit, so the write(2) that crosses it fails (EFBIG, as a full disk fails with
// ENOSPC). GDAL keeps the pixels of a written raster in its block cache and writes them when the
// dataset is flushed or closed: the limit makes that flush or close fail, after every call before
// it went well. POSIX only (RLIMIT_FSIZE): FileSizeLimit::supported is false elsewhere, and the
// tests that need it skip.

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

class FileSizeLimit
{
public:
#ifndef _WIN32
    static constexpr bool supported = true;

    // The signal that says so is ignored: the write fails with an error instead
    explicit FileSizeLimit(unsigned long long bytes)
    {
        getrlimit(RLIMIT_FSIZE, &previous_);
        previousHandler_ = std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit = previous_;
        limit.rlim_cur = static_cast<rlim_t>(bytes);
        setrlimit(RLIMIT_FSIZE, &limit);
    }
    ~FileSizeLimit()
    {
        setrlimit(RLIMIT_FSIZE, &previous_);
        std::signal(SIGXFSZ, previousHandler_);
    }

private:
    rlimit previous_{};
    void (*previousHandler_)(int) = SIG_DFL;
#else
    static constexpr bool supported = false;

    explicit FileSizeLimit(unsigned long long) {}
#endif

public:
    FileSizeLimit(const FileSizeLimit &) = delete;
    FileSizeLimit &operator=(const FileSizeLimit &) = delete;
};

// Room for the header of a GeoTIFF and none of its pixels
constexpr unsigned long long SmallFileLimit = 2048;

#endif // FILE_SIZE_LIMIT_H
