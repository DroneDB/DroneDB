/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cache_cleanup.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <system_error>
#include <vector>

#include "cancellation.h"
#include "exceptions.h"
#include "json.h"
#include "json_options.h"
#include "logger.h"
#include "mio.h"
#include "process_config.h"
#include "raster_artefacts.h"
#include "temp_names.h"

namespace ddb
{
    namespace
    {
        constexpr int kMaxDays = 36500;
        constexpr const char *kTilesFolder = "tiles";
        constexpr const char *kDsmFolder = "dsm_service_cache";
        constexpr const char *kLegacyArtefactsFolder = "artefacts";

        using Clock = fs::file_time_type::clock;

        // The time before which an entry counts as old, for `days`
        Clock::time_point olderThan(int days)
        {
            return Clock::now() - std::chrono::hours(24) * days;
        }

        // The entries of `folder`, listed whole before anything in it is removed (other processes and
        // requests add and remove entries meanwhile); none when it is not there
        std::vector<fs::directory_entry> entriesOf(const fs::path &folder)
        {
            std::vector<fs::directory_entry> entries;
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                entries.push_back(*it);
            return entries;
        }

        bool isFolder(const fs::directory_entry &entry)
        {
            std::error_code ec;
            return entry.symlink_status(ec).type() == fs::file_type::directory && !ec;
        }

        // The last change of `path`: its own, and for a folder the newest of anything below it (a folder whose
        // work still writes in a subfolder is not old). Nullopt when it vanished.
        std::optional<Clock::time_point> lastChange(const fs::path &path)
        {
            std::error_code ec;
            auto newest = fs::last_write_time(path, ec);
            if (ec)
                return std::nullopt;
            if (fs::symlink_status(path, ec).type() != fs::file_type::directory)
                return newest;
            for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code timeEc;
                const auto time = fs::last_write_time(it->path(), timeEc);
                if (!timeEc)
                    newest = std::max(newest, time);
            }
            return newest;
        }

        // The bytes of the files of `path` (links not followed)
        uint64_t bytesOf(const fs::path &path)
        {
            std::error_code ec;
            if (fs::symlink_status(path, ec).type() == fs::file_type::regular)
            {
                const auto size = fs::file_size(path, ec);
                return ec ? 0 : size;
            }
            uint64_t total = 0;
            for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code sizeEc;
                if (it->symlink_status(sizeEc).type() == fs::file_type::regular && !sizeEc)
                {
                    const auto size = it->file_size(sizeEc);
                    total += sizeEc ? 0 : size;
                }
            }
            return total;
        }

        // Removes `path` (counted in `count` once gone, or when only counting)
        void removeEntry(const fs::path &path, bool dryRun, CacheCleanupCount &count)
        {
            cancel::throwIfCanceled();
            const uint64_t bytes = bytesOf(path);
            if (!dryRun)
            {
                std::error_code ec;
                fs::remove_all(path, ec);
                if (ec)
                {
                    // A file a reader still holds (Windows) stays until the next time
                    LOGD << "Cannot remove " << io::toUtf8(path) << " from the cache: " << ec.message();
                    return;
                }
            }
            count.entries++;
            count.bytes += bytes;
        }

        // Removes the entries of `folder` that `wanted` takes and that did not change since `threshold`
        template <typename Wanted>
        void removeOldEntries(const fs::path &folder, Clock::time_point threshold, Wanted wanted, bool dryRun,
                              CacheCleanupCount &count)
        {
            for (const auto &entry : entriesOf(folder))
            {
                if (!wanted(entry))
                    continue;
                const auto changed = lastChange(entry.path());
                if (changed && *changed < threshold)
                    removeEntry(entry.path(), dryRun, count);
            }
        }

        // The folders of rendered tiles: a folder per file version, then a folder per zoom level and per
        // column. A folder not changed since `threshold` goes whole; in a newer one, the same rule applies one
        // level down (what is below a folder that goes is not looked at)
        void removeOldTileFolders(const fs::path &folder, Clock::time_point threshold, bool dryRun,
                                  CacheCleanupCount &count)
        {
            for (const auto &entry : entriesOf(folder))
            {
                if (!isFolder(entry))
                    continue;
                std::error_code ec;
                const auto changed = fs::last_write_time(entry.path(), ec);
                if (ec)
                    continue; // removed by somebody else since the listing
                if (changed < threshold)
                    removeEntry(entry.path(), dryRun, count);
                else
                    removeOldTileFolders(entry.path(), threshold, dryRun, count);
            }
        }

        bool hasTempName(const fs::directory_entry &entry)
        {
            const std::string name = io::toUtf8(entry.path().filename());
            return std::any_of(tempnames::kAll.begin(), tempnames::kAll.end(),
                               [&](std::string_view prefix) { return name.rfind(prefix, 0) == 0; });
        }

        int daysOf(const json &options, const char *name, int fallback)
        {
            const auto it = options.find(name);
            if (it == options.end())
                return fallback;
            if (!it->is_number_integer() || (!it->is_number_unsigned() && it->get<int64_t>() < 0) ||
                it->get<uint64_t>() > static_cast<uint64_t>(kMaxDays))
                throw InvalidArgsException("\"" + std::string(name) + "\" must be a whole number from 0 to " +
                                           std::to_string(kMaxDays));
            return it->get<int>();
        }
    } // namespace

    CacheCleanupResult cleanupCache(const CacheCleanupOptions &options)
    {
        cancel::throwIfCanceled();
        CacheCleanupResult result;
        result.dryRun = options.dryRun;
        for (const char *kind : {"tiles", "downloads", "dsm", "requests", "temp", "artefacts"})
            result.kinds[kind] = CacheCleanupCount();
        // Looked at, not made: a folder that is not there holds nothing
        const fs::path cache = ProcessConfig::get().cacheDir();
        const fs::path tiles = cache / kTilesFolder;

        removeOldTileFolders(tiles, olderThan(options.tileDays), options.dryRun, result.kinds["tiles"]);
        removeOldEntries(
            tiles, olderThan(options.downloadDays), [](const fs::directory_entry &e) { return !isFolder(e); },
            options.dryRun, result.kinds["downloads"]);
        removeOldEntries(
            cache / kDsmFolder, olderThan(options.dsmDays), [](const fs::directory_entry &) { return true; },
            options.dryRun, result.kinds["dsm"]);
        removeOldEntries(
            raster::requestsDir(), olderThan(options.tempDays),
            [](const fs::directory_entry &e) { return !raster::isLiveRequestFolder(e.path()); }, options.dryRun,
            result.kinds["requests"]);
        std::error_code ec;
        const fs::path temp = fs::temp_directory_path(ec);
        if (!ec)
            removeOldEntries(temp, olderThan(options.tempDays), hasTempName, options.dryRun, result.kinds["temp"]);
        if (fs::exists(fs::symlink_status(cache / kLegacyArtefactsFolder, ec)))
            removeEntry(cache / kLegacyArtefactsFolder, options.dryRun, result.kinds["artefacts"]);

        for (const auto &[kind, count] : result.kinds)
        {
            if (count.entries != 0)
            {
                LOGD << (options.dryRun ? "Would remove " : "Removed ") << count.entries << " " << kind
                     << " entries from the cache (" << count.bytes / (1024 * 1024) << " MiB)";
            }
        }
        return result;
    }

    CacheCleanupOptions cacheCleanupOptionsFromJson(const json &options)
    {
        if (!options.is_object())
            throw InvalidArgsException("The cache cleanup options must be a JSON object");
        rejectUnknownMembers(options, {"tileDays", "downloadDays", "dsmDays", "tempDays", "dryRun"},
                             "cache cleanup option");
        CacheCleanupOptions result;
        result.tileDays = daysOf(options, "tileDays", result.tileDays);
        result.downloadDays = daysOf(options, "downloadDays", result.downloadDays);
        result.dsmDays = daysOf(options, "dsmDays", result.dsmDays);
        result.tempDays = daysOf(options, "tempDays", result.tempDays);
        if (const auto it = options.find("dryRun"); it != options.end())
        {
            if (!it->is_boolean())
                throw InvalidArgsException("\"dryRun\" must be true or false");
            result.dryRun = it->get<bool>();
        }
        return result;
    }

    json toJson(const CacheCleanupResult &result)
    {
        json kinds = json::object();
        for (const auto &[kind, count] : result.kinds)
            kinds[kind] = {{"entries", count.entries}, {"bytes", count.bytes}};
        return {{"dryRun", result.dryRun}, {"kinds", kinds}};
    }
} // namespace ddb
