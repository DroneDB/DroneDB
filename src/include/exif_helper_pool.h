/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIF_HELPER_POOL_H
#define EXIF_HELPER_POOL_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "exif_metadata.h"
#include "exif_protocol.h"
#include "fs.h"
#include "helper_pool.h"

namespace ddb
{
    namespace exif
    {
        // The program that reads and writes image metadata, and the environment variable that
        // names its file (tools::findTool takes both)
        inline constexpr const char *kMetadataUtilName = "metadata-util";
        inline constexpr const char *kMetadataUtilEnv = "DDB_METADATA_UTIL_PATH";

        // What a HelperMetadataReader can be told. The defaults are what the process uses; the
        // tests change them.
        struct HelperMetadataReaderOptions
        {
            // The metadata-util executable, empty when there is none. Default: tools::findTool
            std::function<fs::path()> locate;
            // The most helpers at once. Default: ProcessConfig::cpuPoolSize() + 1
            std::function<size_t()> limit;
            // The time the idle limit counts in. Default: the steady clock
            std::function<std::chrono::steady_clock::time_point()> now;

            std::chrono::milliseconds idleTimeout = std::chrono::seconds(60);
            // To write a request, and from the moment the caller waits for the response (right
            // after the request, except for a read begin() started) to the end of its line
            std::chrono::milliseconds requestTimeout = std::chrono::seconds(30);
            // To start `--version` and have it answer
            std::chrono::milliseconds versionTimeout = std::chrono::seconds(10);
            // The time all helpers get together to end after their input is closed
            std::chrono::milliseconds closeTimeout = std::chrono::seconds(2);
            // A longer response line is a broken helper
            size_t maxResponseBytes = 16 * 1024 * 1024;
        };

        // Reads and writes metadata through a pool of metadata-util processes (`metadata-util --serve`,
        // vendor/metadata-util). The pool itself (helper_pool.h) is generic; this class is the
        // metadata protocol on top of it: the requests and responses (exif_protocol.h), the
        // accepted protocol versions, the mapping of a broken helper to an IndexException.
        //
        //   - A helper serves one request at a time and is held by one caller at a time; between
        //     requests it waits idle in the pool.
        //   - The pool holds at most ProcessConfig::cpuPoolSize() + 1 helpers (read whenever a
        //     caller takes one, so DDBConfigure applies at once). A caller that finds none idle
        //     and the pool full waits its turn: callers are served in the order they arrived.
        //     begin() does not wait: it leaves the read to get() (see there).
        //   - A helper is started only when a caller needs one and none is idle, after
        //     `metadata-util --version` reported a protocol this client implements. The answer is
        //     kept for the program file as it is (path, size, modification time): it is asked
        //     once, and again when the file changes.
        //   - An idle helper that has ended by itself (killed by the system or a user) is
        //     dropped when a caller takes it, and the caller gets a new one.
        //   - The program is looked up (tools::findTool, DDB_METADATA_UTIL_PATH) whenever a caller takes a
        //     helper; idle helpers of another program are closed.
        //   - An idle helper is closed when it has been idle for more than a minute; the check
        //     runs when a helper is taken or returned (no timer thread).
        //   - A helper that dies, hangs, or sends what the protocol does not allow is killed and
        //     dropped, and the request fails (read: IndexException naming the file). The other
        //     helpers do not notice, and the next request starts a new one.
        //   - A helper closed by the pool that does not exit with 0 (a sanitizer build reports a
        //     leak that way) is a WARNING in the log, with its status and the end of its stderr.
        //
        // The pool never holds its lock while it starts a process or talks to one.
        class HelperMetadataReader final : public IMetadataReader
        {
          public:
            using Clock = std::chrono::steady_clock;

            using Options = HelperMetadataReaderOptions;

            explicit HelperMetadataReader(Options options = Options());
            // Closes the idle helpers. Helpers held by callers must not exist (no reader is
            // destroyed while it is used)
            ~HelperMetadataReader() override;

            HelperMetadataReader(const HelperMetadataReader &) = delete;
            HelperMetadataReader &operator=(const HelperMetadataReader &) = delete;

            // The reader of the process (metadataReader() of exif_metadata.h). It is never
            // destroyed: hosts such as the Registry load the library for the life of the process
            // and never finalize it, and the helpers end with their input anyway.
            static HelperMetadataReader &instance();
            // shutdown() of instance() when it exists; 0 otherwise
            static size_t shutdownInstance();

            // Takes a helper (waiting its turn), sends the request and reads the response
            ReadResult read(const fs::path &file) override;
            // Never waits for a helper. When one is idle, or the pool has room to start one, and
            // no caller is queued, it takes it and writes the request; the helper stays held by
            // the PendingRead until get() read its response. Otherwise it sends nothing, and
            // get() makes the read, waiting its turn like read(): a caller about to hash a large
            // file does not wait for a helper before it can start, nor holds one through the
            // hash while others wait, and does not pass the callers already queued.
            //
            // A PendingRead dropped before get() kills the helper it holds instead of reading
            // the response it is working on: its place is free at once, nothing waits for a
            // helper that may hang, and no helper whose response is unread can be reused.
            PendingRead begin(const fs::path &file) override;
            WriteResult write(const fs::path &file, const TagAssignments &set) override;

            // Closes the idle helpers: their input first, then one wait of at most closeTimeout
            // for all of them, then a kill for the ones still there. Helpers held by a caller
            // are closed when it returns them. The pool can be used again afterwards. Returns the
            // number of helpers closed.
            size_t shutdown() { return pool_.shutdown(); }

            // What the tests observe the pool through: how many processes it started and how
            // many requests it sent over the life of the object, the helpers waiting and the
            // helpers held right now, the callers queued for a helper, and the process ids of the
            // idle helpers. awaitWaiters() blocks until `count` callers are queued (false after
            // the timeout), so a test queues callers in a known order without sleeping.
            size_t startedHelpers() const { return pool_.startedHelpers(); }
            size_t requestsSent() const { return pool_.requestsSent(); }
            size_t idleHelpers() const { return pool_.idleHelpers(); }
            size_t busyHelpers() const { return pool_.busyHelpers(); }
            size_t queuedCallers() const { return pool_.queuedCallers(); }
            bool awaitWaiters(size_t count, std::chrono::milliseconds timeout) const
            {
                return pool_.awaitWaiters(count, timeout);
            }
            std::vector<unsigned long> idleHelperPids() const { return pool_.idleHelperPids(); }

          private:
            struct Exchange;
            class PendingHelperRead;

            using Lease = helper::Pool::Lease;

            // One request and its response on a helper of the pool: receive(send(...)). Both
            // throw helper::Failure (after the helper is discarded and logged) when the helper
            // breaks; the failure of a file (ok: false with an error that names the file) is a
            // response.
            protocol::Response serve(const fs::path &file, protocol::Operation operation,
                                     const std::string &request, int64_t id);
            // Writes the request to the helper of `lease`
            Exchange send(Lease lease, const fs::path &file, protocol::Operation operation,
                          const std::string &request, int64_t id);
            // Reads and checks the response; the helper goes back to the pool when it is good
            protocol::Response receive(Exchange &exchange);

            // The time a request may take to be written, and a response to arrive
            const std::chrono::milliseconds requestTimeout_;
            helper::Pool pool_;
            std::atomic<int64_t> nextId_{1};
        };

    } // namespace exif
} // namespace ddb

#endif // EXIF_HELPER_POOL_H
