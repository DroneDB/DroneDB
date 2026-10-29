/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_HELPER_H
#define RASTER_HELPER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "exceptions.h"
#include "fs.h"
#include "helper_pool.h"
#include "json.h"
#include "process_runner.h"
#include "raster_ops.h"
#include "raster_protocol.h"

namespace ddb
{
    struct FingerprintContext;

    // The host side of the raster helper (gdal-util, apps/gdal-util/PROTOCOL.md): the program that
    // reads the files users supplied with GDAL, so a defect in a driver or a hostile file runs
    // in a process of its own and not in the host. The host sends requests and understands the
    // answers (raster_protocol.h); it never opens such a file itself.
    namespace raster
    {
        // The program, and the environment variable that names its file (tools::findTool takes both)
        inline constexpr const char *kGdalUtilName = "gdal-util";
        inline constexpr const char *kGdalUtilEnv = "DDB_GDAL_UTIL_PATH";

        // How long a request may take, from the moment it is written to the end of its response.
        // A longer one is a hung helper: killed, and the request fails with a timeout.
        inline constexpr std::chrono::seconds kProbeTimeout{30};
        inline constexpr std::chrono::seconds kThumbnailTimeout{30};
        inline constexpr std::chrono::seconds kConversionTimeout{120};

        // The longest a build (cog, vector) may run in total, whatever the program prints: the
        // variable that sets it in seconds, and its default. The host ends the program at that
        // time and the program limits its own cpu time by it (tools::toolMaxRuntime reads the
        // variable).
        inline constexpr const char *kGdalUtilMaxSecondsEnv = "DDB_GDAL_UTIL_MAX_SECONDS";
        inline constexpr std::chrono::seconds kGdalUtilDefaultMaxRuntime{12 * 60 * 60};

        // The longest thumbnail file the host accepts from the helper. A WebP of the largest
        // size the format holds (16383 x 16383) is far below it; a bigger file is not a thumbnail.
        inline constexpr size_t kMaxThumbnailBytes = 64u * 1024 * 1024;

        // The helper could not read the file within its limits: it ran out of memory or cpu time,
        // ended without an answer, did not answer in time, or answered what the protocol does not
        // allow. The file may well be fine: the index keeps the entry, without what the helper
        // would have found, and says so in its warning (notReadWarning()). `reason` says what
        // happened, for people.
        class RasterNotReadException : public GDALException
        {
          public:
            RasterNotReadException(ErrorCode code, const std::string &reason) : GDALException(reason), code_(code) {}
            // Limit, Crashed or Timeout
            ErrorCode code() const noexcept { return code_; }

          private:
            ErrorCode code_;
        };

        // The warning of an entry whose file the helper could not read
        std::string notReadWarning(const std::string &reason);

        // What the host asks the raster helper. Each operation takes a file a user supplied and
        // returns plain data or writes the file it is given; a failure is thrown as follows:
        //   - RasterOpException, code Unreadable or Policy: GDAL cannot open the file (as what the
        //     operation needs) or the open policy refuses it; code Internal: the helper failed
        //     for a reason of its own, with the message it gave;
        //   - RasterNotReadException: the limits above;
        //   - FSException with the "no space on device" code: the device the output goes to is full;
        //   - HelperUnavailableException: the program is missing or speaks another protocol major;
        //   - CanceledException: the caller's work was canceled, the helper is gone.
        // The three exceptions that carry a GDAL message are GDALExceptions, so a caller that does
        // not care about the difference treats them as what GDAL threw before.
        class IRasterHelper
        {
          public:
            virtual ~IRasterHelper() = default;

            // What GDAL finds in a raster or a vector file, as the entry of the file needs it
            virtual ProbeResult probe(const fs::path &path, const ProbeParams &params) = 0;
            // Writes the thumbnail (WebP) of a photo or a raster to `out`, in a folder only this
            // request writes to; the caller checks the file before it trusts it (renderThumbnailWebp())
            virtual void thumbnail(const fs::path &path, const fs::path &out, const ThumbnailParams &params) = 0;
            // The operations below take the members the protocol defines for them in `params`
            virtual void normalize(const fs::path &path, const fs::path &out, const json &params) = 0;
            virtual void geoproject(const fs::path &path, const fs::path &out, const json &params) = 0;
            // Long conversions: one process each, which writes progress as it goes. A full device
            // is a RetryableBuildException (NoSpaceOnDevice), as any build reports it; an idle
            // process one with the reason IdleTimeout.
            virtual void buildCog(const fs::path &path, const fs::path &out, const json &params,
                                  const ProgressCallback &progress) = 0;
            virtual void buildVector(const fs::path &path, const fs::path &out, const json &params,
                                     const ProgressCallback &progress) = 0;
        };

        // What a GdalUtilHelper can be told. The defaults are what the process uses; the tests
        // change them.
        struct GdalUtilHelperOptions
        {
            // The gdal-util executable, empty when there is none. Default: tools::findTool
            std::function<fs::path()> locate;
            // The most helpers at once. Default: ProcessConfig::cpuPoolSize() + 1
            std::function<size_t()> limit;
            // The time the idle limit counts in. Default: the steady clock
            std::function<std::chrono::steady_clock::time_point()> now;

            std::chrono::milliseconds idleTimeout = std::chrono::seconds(60);
            std::chrono::milliseconds probeTimeout = kProbeTimeout;
            std::chrono::milliseconds thumbnailTimeout = kThumbnailTimeout;
            std::chrono::milliseconds conversionTimeout = kConversionTimeout;
            // To start `--version` and have it answer
            std::chrono::milliseconds versionTimeout = std::chrono::seconds(10);
            // The time all helpers get together to end after their input is closed
            std::chrono::milliseconds closeTimeout = std::chrono::seconds(2);
            // A longer response line is a broken helper
            size_t maxResponseBytes = kMaxLineBytes;
            // How the one-shot builds start the program. Default: process::systemProcessRunner()
            const process::IProcessRunner *runner = nullptr;
        };

        // The raster helper over a pool of `gdal-util --serve` processes (helper_pool.h) for the
        // short operations, and one `gdal-util --request` process for each build.
        //
        //   - The pool holds at most ProcessConfig::cpuPoolSize() + 1 helpers; a helper serves one
        //     request at a time. Each request is served in a worker of the helper that ends with
        //     the request (apps/gdal-util/PROTOCOL.md), so a defect in a driver never meets a
        //     second file.
        //   - The program is looked up (tools::findTool, DDB_GDAL_UTIL_PATH) whenever a caller takes
        //     a helper, and is started only after `gdal-util --version` reported a protocol major
        //     this client implements.
        //   - Helpers start with MALLOC_ARENA_MAX=2 (a bounded heap per worker) and PROJ_NETWORK=OFF
        //     (a worker never fetches a grid from the network).
        //   - A helper that dies, hangs, or sends what the protocol does not allow is killed and
        //     dropped, the failure is logged (operation, error code, file) and the request fails
        //     with RasterNotReadException; the next request starts a new helper.
        //
        // The pool never holds its lock while it starts a process or talks to one.
        class GdalUtilHelper final : public IRasterHelper
        {
          public:
            using Options = GdalUtilHelperOptions;

            explicit GdalUtilHelper(Options options = Options());
            // Closes the idle helpers. Helpers held by callers must not exist
            ~GdalUtilHelper() override;

            GdalUtilHelper(const GdalUtilHelper &) = delete;
            GdalUtilHelper &operator=(const GdalUtilHelper &) = delete;

            // The helper of the process (rasterHelper()). It is never destroyed: hosts such as the
            // Registry load the library for the life of the process and never finalize it, and the
            // helpers end with their input anyway.
            static GdalUtilHelper &instance();
            // shutdown() of instance() when it exists; 0 otherwise
            static size_t shutdownInstance();

            ProbeResult probe(const fs::path &path, const ProbeParams &params) override;
            void thumbnail(const fs::path &path, const fs::path &out, const ThumbnailParams &params) override;
            void normalize(const fs::path &path, const fs::path &out, const json &params) override;
            void geoproject(const fs::path &path, const fs::path &out, const json &params) override;
            void buildCog(const fs::path &path, const fs::path &out, const json &params,
                          const ProgressCallback &progress) override;
            void buildVector(const fs::path &path, const fs::path &out, const json &params,
                             const ProgressCallback &progress) override;

            // Closes the idle helpers (their input first, then a kill for the ones that stay); the
            // helper can be used again afterwards. Returns the number closed.
            size_t shutdown() { return pool_.shutdown(); }

            // What the tests observe the pool through, as helper::Pool does
            size_t startedHelpers() const { return pool_.startedHelpers(); }
            size_t requestsSent() const { return pool_.requestsSent(); }
            size_t idleHelpers() const { return pool_.idleHelpers(); }
            size_t busyHelpers() const { return pool_.busyHelpers(); }
            std::vector<unsigned long> idleHelperPids() const { return pool_.idleHelperPids(); }

          private:
            // Checks the result of a success while the helper is still held (throws ProtocolError for
            // one the protocol does not allow: the helper is broken then)
            using ResponseCheck = std::function<void(const Response &)>;

            // One request on a helper of the pool and its response. A helper that breaks makes the
            // request fail with RasterNotReadException; a response that is a failure is returned
            // for the caller to raise.
            Response serve(const char *op, const fs::path &path, const fs::path &out, json params,
                           std::chrono::milliseconds timeout, const ResponseCheck &accept);
            // One `gdal-util --request` process
            void runOneShot(const char *op, const fs::path &path, const fs::path &out, json params,
                            const ProgressCallback &progress);

            const Options options_;
            helper::Pool pool_;
            std::atomic<int64_t> nextId_{1};
        };

        // The raster helper of the process: the one a ScopedRasterHelper installed, else the
        // GdalUtilHelper of the process
        IRasterHelper &rasterHelper();

        // While alive, rasterHelper() is `helper` (tests); the previous one comes back after.
        // Installations nest in the order of their lifetimes.
        class ScopedRasterHelper
        {
          public:
            explicit ScopedRasterHelper(IRasterHelper &helper);
            ~ScopedRasterHelper();
            ScopedRasterHelper(const ScopedRasterHelper &) = delete;
            ScopedRasterHelper &operator=(const ScopedRasterHelper &) = delete;

          private:
            IRasterHelper *previous_;
        };

        // Closes the helpers of the process (DDBShutdown): no gdal-util process of this host is
        // left. Returns the number closed.
        size_t closeRasterHelpers();

        // ---- what the host does with the answers

        // What the host learned of a file by probing it
        struct ProbeOutcome
        {
            ProbeKind kind = ProbeKind::Raster;
            fs::path path;
            // Set when the helper read the file
            std::optional<ProbeResult> result;
            // Set when the helper could not read the file within its limits: the reason. With
            // neither, GDAL cannot open the file (or the open policy refuses it): the entry falls
            // back as for any file GDAL does not know.
            std::optional<std::string> notRead;
        };

        // Probes the file once per pass over it: the answer is kept in the context that the
        // detection and the metadata step of an entry share (they both ask), and a context that is
        // asked again for the same file and kind answers from it. A failure of the helper itself
        // (a response that says "internal", a missing program, a canceled run) is thrown.
        // @throws RasterOpException (code Internal), HelperUnavailableException, FSException,
        // CanceledException
        const ProbeOutcome &probeFile(FingerprintContext &ctx, const fs::path &path, const ProbeParams &params);

        // The thumbnail of a photo or a raster as WebP bytes, made by the helper and checked: the
        // helper writes into a folder that only this request writes to (made here, mode 0700), and
        // its file counts only when it is a WebP of a size a thumbnail can have. A file that is
        // not one is never returned (so never lands in the thumbnail cache). A URL is downloaded
        // first: the helper reads files only.
        // @throws InvalidArgsException for a size that is not positive; the exceptions of
        // IRasterHelper; GDALException for an output that is not a thumbnail
        std::string renderThumbnailWebp(const fs::path &path, const ThumbnailParams &params);

        // Whether the bytes are a WebP file (RIFF container whose size is the file's) at most
        // kMaxThumbnailBytes long
        bool looksLikeThumbnail(const std::string &bytes);

        // Downloads a URL into `target` (a file in a folder the caller made); a failed or
        // partial transfer leaves nothing at `target`.
        // @throws NetException, FSException
        void downloadInput(const std::string &url, const fs::path &target);

    } // namespace raster
} // namespace ddb

#endif // RASTER_HELPER_H
