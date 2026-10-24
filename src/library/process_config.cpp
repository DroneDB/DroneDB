/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "process_config.h"

#include <array>
#include <string>
#include <string_view>

#include "build_budget.h"
#include "env.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "userprofile.h"

namespace ddb
{
    namespace
    {
        // An integer setting: its DDBConfigure name, its environment variable, the values it
        // accepts and its built-in default
        struct IntegerSetting
        {
            std::string_view name;
            const char *env;
            uint64_t min;
            uint64_t max;
            uint64_t (*builtIn)();
        };

        // The free-disk floor in bytes must fit in 64 bits: 10^13 MiB (about 10 EiB) does
        constexpr uint64_t MaxFreeDiskMb = 10'000'000'000'000ULL;

        // Indexes into kIntegerSettings and ProcessConfig::integers_
        constexpr size_t CpuPoolSize = 0;
        constexpr size_t IoThreads = 1;
        constexpr size_t MinFreeDiskMb = 2;
        constexpr size_t GdalCacheMb = 3;

        // GDAL's block cache: 16 MiB to 1 TiB (1 TiB in bytes fits in 64 bits)
        constexpr uint64_t MinGdalCacheMb = 16;
        constexpr uint64_t MaxGdalCacheMb = 1024 * 1024;

        const std::array<IntegerSetting, 4> kIntegerSettings = {{
            // Every core: the calling thread plus cores - 1 workers, as the parallel sections
            // used before the pool existed (ALL_CPUS)
            {"cpuPoolSize", "DDB_CPU_POOL_SIZE", 0, 4096,
             [] { return static_cast<uint64_t>(parallel::hardwareThreads() - 1); }},
            // A few parallel readers help on a RAID array without thrashing a single disk
            {"ioThreads", "DDB_IO_THREADS", 1, 256, [] { return uint64_t{4}; }},
            // Low enough never to trip on a healthy host, high enough to stop a build from
            // starting on an essentially full disk
            {"minFreeDiskMb", "DDB_MIN_FREE_DISK_MB", 0, MaxFreeDiskMb, [] { return uint64_t{512}; }},
            // GDAL's own default is 5% of the RAM (6 GB on a 128 GB host), shared by tile serving
            // and builds: this is the bound libddb puts on it unless the host sets a size
            {"gdalCacheMb", "DDB_GDAL_CACHE_MB", MinGdalCacheMb, MaxGdalCacheMb, [] { return uint64_t{1024}; }},
        }};

        constexpr std::string_view CacheDirName = "cacheDir";

        const char *sourceName(ConfigSource source)
        {
            switch (source)
            {
            case ConfigSource::Configure:
                return "configure";
            case ConfigSource::Environment:
                return "environment";
            case ConfigSource::Default:
                break;
            }
            return "default";
        }

        std::string knownNames()
        {
            std::string names(CacheDirName);
            for (const auto &setting : kIntegerSettings)
                names.append(", ").append(setting.name);
            return names;
        }

        uint64_t integerValue(const IntegerSetting &setting, const json &value)
        {
            const auto invalid = [&setting]() {
                return InvalidArgsException(std::string(setting.name) + " must be an integer from " +
                                            std::to_string(setting.min) + " to " + std::to_string(setting.max) +
                                            ", or null");
            };
            // A JSON integer >= 0 parses as unsigned; a negative one, a float, a string or a
            // boolean is invalid
            if (!value.is_number_unsigned())
                throw invalid();
            const auto v = value.get<uint64_t>();
            if (v < setting.min || v > setting.max)
                throw invalid();
            return v;
        }

        fs::path cacheDirValue(const json &value)
        {
            if (!value.is_string() || value.get_ref<const std::string &>().empty())
                throw InvalidArgsException(std::string(CacheDirName) + " must be an absolute folder path, or null");
            fs::path dir = io::fromUtf8(value.get_ref<const std::string &>());
            if (!dir.is_absolute())
                throw InvalidArgsException(std::string(CacheDirName) + " must be an absolute folder path: " +
                                           value.get<std::string>());
            return dir.lexically_normal();
        }
    } // namespace

    ProcessConfig &ProcessConfig::get()
    {
        // Never destroyed: libddb code running during static destruction may still read it
        static ProcessConfig *const instance = new ProcessConfig();
        return *instance;
    }

    void ProcessConfig::configure(const json &changes)
    {
        if (!changes.is_object())
            throw InvalidArgsException("The configuration must be a JSON object");

        // Check everything first: an invalid member changes nothing
        std::array<std::optional<std::optional<uint64_t>>, IntegerSettings> integers;
        std::optional<std::optional<fs::path>> cacheDir;
        for (const auto &[name, value] : changes.items())
        {
            if (name == CacheDirName)
            {
                cacheDir = value.is_null() ? std::optional<fs::path>() : std::optional<fs::path>(cacheDirValue(value));
                continue;
            }
            size_t index = 0;
            while (index < kIntegerSettings.size() && kIntegerSettings[index].name != name)
                index++;
            if (index == kIntegerSettings.size())
                throw InvalidArgsException("Unknown setting \"" + name + "\" (known: " + knownNames() + ")");
            integers[index] = value.is_null() ? std::optional<uint64_t>()
                                              : std::optional<uint64_t>(integerValue(kIntegerSettings[index], value));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < IntegerSettings; i++)
                if (integers[i])
                    integers_[i] = *integers[i];
            if (cacheDir)
                cacheDir_ = *cacheDir;
        }
        LOGD << "Process configuration changed: " << changes.dump();

        // GDAL's cache is process state, not a value read when needed: a change is applied now
        if (integers[GdalCacheMb])
            applyGdalCacheSetting();
    }

    ProcessConfig::Integer ProcessConfig::integer(size_t index) const
    {
        const auto &setting = kIntegerSettings[index];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (integers_[index])
                return {*integers_[index], ConfigSource::Configure};
        }
        // Invalid values are logged at debug level only: this runs on every read of a setting
        const auto fromEnv = env::readInteger(setting.env, setting.min, setting.max);
        if (fromEnv.state == env::Integer::State::Valid)
            return {fromEnv.value, ConfigSource::Environment};
        if (fromEnv.state == env::Integer::State::Invalid)
        {
            LOGD << "Ignoring " << setting.env << "=\"" << fromEnv.raw << "\": expected an integer from "
                 << setting.min << " to " << setting.max;
        }
        return {setting.builtIn(), ConfigSource::Default};
    }

    ProcessConfig::Path ProcessConfig::cacheDirWithSource() const
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cacheDir_)
                return {*cacheDir_, ConfigSource::Configure};
        }
        bool fromDdbHome = false;
        fs::path dir = UserProfile::defaultProfileDir(&fromDdbHome);
        return {std::move(dir), fromDdbHome ? ConfigSource::Environment : ConfigSource::Default};
    }

    json ProcessConfig::effective() const
    {
        json settings = json::object();
        json sources = json::object();
        for (size_t i = 0; i < IntegerSettings; i++)
        {
            const auto value = integer(i);
            const std::string name(kIntegerSettings[i].name);
            settings[name] = value.value;
            sources[name] = sourceName(value.source);
        }
        const auto dir = cacheDirWithSource();
        settings[std::string(CacheDirName)] = io::toUtf8(dir.value);
        sources[std::string(CacheDirName)] = sourceName(dir.source);
        return {{"settings", settings},
                {"sources", sources},
                {"hardwareThreads", parallel::hardwareThreads()}};
    }

    size_t ProcessConfig::cpuPoolSize() const
    {
        return static_cast<size_t>(integer(CpuPoolSize).value);
    }

    size_t ProcessConfig::ioThreads() const
    {
        return static_cast<size_t>(integer(IoThreads).value);
    }

    uint64_t ProcessConfig::minFreeDiskMb() const
    {
        return integer(MinFreeDiskMb).value;
    }

    uint64_t ProcessConfig::gdalCacheMb() const
    {
        return integer(GdalCacheMb).value;
    }

    ConfigSource ProcessConfig::gdalCacheSource() const
    {
        return integer(GdalCacheMb).source;
    }

    fs::path ProcessConfig::cacheDir() const
    {
        return cacheDirWithSource().value;
    }

} // namespace ddb
