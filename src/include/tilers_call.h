/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERS_CALL_H
#define TILERS_CALL_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ddbt/ddbt.h"

namespace ddb::tilers
{
    // Throws the libddb exception of a failed ddb-tilers call, with its message (ddbtLastError):
    // InvalidArgsException for bad arguments, RefusedArgumentException for one the input cannot
    // honour (a formula the raster cannot compute), FSException for a missing input, CanceledException,
    // GDALException for anything else (what libddb's own raster code throws for a raster it cannot
    // read or render). Nothing when rc is DDBT_ERR_NONE.
    void check(ddbtErr rc);

    // Throws what the end of a build of ddb-tilers (ddbtBuildCog2, ddbtBuildCopc2) means: a full
    // device is a RetryableBuildException (NoSpaceOnDevice) whose message starts with `what` ("The
    // COG build"); a build whose work was canceled (cancellation.h) a CanceledException, whatever
    // rc is; an input the builder cannot build (DDBT_ERR_INVALIDARGS, DDBT_ERR_NOT_IMPLEMENTED)
    // what `throwUnbuildable` throws with the message of ddb-tilers (the exception a caller
    // expects for a file it cannot read); anything else what check() throws.
    void checkBuild(ddbtErr rc, const std::string &what,
                    const std::function<void(const std::string &)> &throwUnbuildable);

    // Forwards the progress of a build of ddb-tilers to `report` at every whole percent, and logs
    // it every 5% ("<label> progress: 40%"). `forward` is the ddbtProgressCallback, with this
    // object as its user data; ddb-tilers calls it one call at a time, from any of its threads.
    class BuildProgress
    {
      public:
        BuildProgress(std::string label, std::function<void(double)> report);

        static void forward(double fraction, const char *phase, void *self);

      private:
        void onProgress(double fraction);

        const std::string label_;
        const std::function<void(double)> report_;
        std::atomic<int> lastPct_{0};
    };

    // Makes ddb-tilers read the files it tiles at http(s) URLs (COPC point clouds) with range
    // requests of libddb's HTTP client (defaultHttpClient): the version of a file is its ETag, else
    // its Last-Modified date, so a changed file is opened again. Called once by registerProcess.
    void useHttpClientForRemoteReads();

    // A buffer or string ddb-tilers returned, released with ddbtFree
    struct FreeBuffer
    {
        void operator()(void *p) const noexcept { ddbtFree(p); }
    };
    using Bytes = std::unique_ptr<uint8_t, FreeBuffer>;
    using String = std::unique_ptr<char, FreeBuffer>;
} // namespace ddb::tilers

#endif // TILERS_CALL_H
