/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PROCESS_CONFIG_H
#define PROCESS_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "fs.h"
#include "json_fwd.h"

namespace ddb
{

    // Where the value of a setting comes from, by precedence
    enum class ConfigSource
    {
        Configure,    // DDBConfigure
        Environment,  // the setting's environment variable
        Default       // libddb's built-in default
    };

    // The process configuration (DQ-15, XI-06): the settings a host passes to DDBConfigure
    // (ddb.h documents them, with their environment variables and defaults). It is the only
    // place that reads them: a value set with configure() wins over the environment variable,
    // which wins over the built-in default. The environment is read whenever a value is needed,
    // as before, so a variable changed at run time still counts while no DDBConfigure value
    // hides it.
    //
    // Thread-safe: configure() may run at any time, concurrently with any reader. A change
    // applies to what reads the setting after configure() returned.
    class ProcessConfig
    {
    public:
        static ProcessConfig &get();

        // Applies the members of `changes` (a JSON object): a value sets the setting, null puts
        // it back to its environment variable or default. Every member is checked first: on an
        // unknown name or an invalid value it throws InvalidArgsException and changes nothing.
        void configure(const json &changes);

        // {"settings": {name: value}, "sources": {name: "configure"|"environment"|"default"},
        //  "hardwareThreads": n}: the value of every setting and where it comes from
        json effective() const;

        // Size of the process-wide CPU token pool (parallel.h): worker threads that may run
        // beside the calling threads, across every libddb call at once
        size_t cpuPoolSize() const;
        // Threads of one I/O-bound parallel section (hashing files, deleting build folders)
        size_t ioThreads() const;
        // Free space a build (or a copy of build artifacts) needs on its volume, in MiB; 0 turns
        // the check off
        uint64_t minFreeDiskMb() const;
        // The folder of libddb's caches (thumbnails, tiles, index templates, DSM tiles); not
        // created here (UserProfile::getCacheDir creates it)
        fs::path cacheDir() const;

        ProcessConfig(const ProcessConfig &) = delete;
        ProcessConfig &operator=(const ProcessConfig &) = delete;

    private:
        ProcessConfig() = default;

        // One entry per integer setting (kIntegerSettings in process_config.cpp)
        static constexpr size_t IntegerSettings = 3;

        struct Integer
        {
            uint64_t value;
            ConfigSource source;
        };
        struct Path
        {
            fs::path value;
            ConfigSource source;
        };

        Integer integer(size_t index) const;
        Path cacheDirWithSource() const;

        mutable std::mutex mutex_;
        std::optional<uint64_t> integers_[IntegerSettings];
        std::optional<fs::path> cacheDir_;
    };

} // namespace ddb

#endif // PROCESS_CONFIG_H
