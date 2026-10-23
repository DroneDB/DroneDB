/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Turns what a caller names (files, folders, glob patterns) into the paths to index or describe.
#include "dbops.h"
#include "dataset_walk.h"

#include "entry_path.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"

#include <unordered_map>
#include <unordered_set>

#include <glob.hpp>

namespace ddb
{

    // Computes a list of paths inside rootDirectory
    // all paths must be subfolders/files within rootDirectory
    // or an exception is thrown
    // If includeDirs is true and the list includes paths to directories that are in
    // paths eg. if path/to/file is in paths, both "path/" and "path/to" are
    // included in the result.
    // ".ddb" files/dirs are always ignored and skipped.
    // If a directory is in the input paths, they are included regardless of
    // includeDirs
    std::vector<fs::path> getIndexPathList(const fs::path &rootDirectory,
                                           const std::vector<std::string> &paths,
                                           bool includeDirs, bool recursive)
    {
        std::vector<fs::path> result;
        std::unordered_map<std::string, bool> directories;

        // A file can be listed explicitly and found again under a listed folder
        std::unordered_set<std::string> files;
        const auto addFile = [&result, &files](const fs::path &f)
        {
            if (files.insert(io::toUtf8(f)).second)
                result.push_back(f);
        };

        io::Path rootDir = rootDirectory;

        // Records the folders between `path` and the root. Stops at a folder already
        // recorded: its own parents were recorded with it.
        const auto addParents = [&rootDir, &directories](fs::path path)
        {
            while (path.has_parent_path() && path != path.parent_path())
            {
                path = path.parent_path();
                if (directories.count(io::toUtf8(path)) > 0 || !rootDir.isParentOf(path))
                    break;
                directories[io::toUtf8(path)] = true;
            }
        };

        for (const std::string &p : paths)
        {
            if (p.empty())
                throw FSException("Some paths are empty");
        }

        if (!io::Path(rootDirectory).hasChildren(paths))
        {
            throw FSException("Some paths are not contained within: " +
                              io::toUtf8(rootDirectory) + ". Did you run ddb init?");
        }

        for (const auto &pathUtf8 : paths)
        {
            fs::path p = io::fromUtf8(pathUtf8);
            if (isReservedSegment(p.filename()))
                continue;

            if (fs::is_directory(p))
            {
                if (recursive)
                {
                    walkDataset(p, {}, [&](const fs::directory_entry &entry)
                    {
                        const fs::path &rp = entry.path();

                        // The iterator knows the type from the directory listing: no stat
                        if (entry.is_directory() && includeDirs)
                        {
                            directories[io::toUtf8(rp)] = true;
                        }
                        else
                        {
                            addFile(rp);
                        }

                        if (includeDirs)
                            addParents(rp);
                    });
                }

                // The dataset root is not an entry (`ddb add .` used to index it as "")
                if (rootDir.isParentOf(p))
                    directories[io::toUtf8(p)] = true;
                if (includeDirs)
                    addParents(p);
            }
            else if (fs::exists(p))
            {
                // File
                addFile(p);

                if (includeDirs)
                    addParents(p);
            }
            else
            {
                throw FSException("Path does not exist: " + io::toUtf8(p));
            }
        }

        for (auto [fst, snd] : directories)
        {
            result.push_back(io::fromUtf8(fst));
        }

        return result;
    }

    std::vector<fs::path> getPathList(const std::vector<std::string> &paths,
                                      bool includeDirs, int maxDepth, bool includeFiles)
    {
        std::vector<fs::path> result;

        // maxDepth: N > 0 lists N levels below a folder, -1 its children only, anything else
        // the whole tree
        WalkOptions options;
        options.levels = maxDepth > 0 ? maxDepth : (maxDepth == -1 ? 1 : 0);
        options.skipHiddenAndSystem = true;

        for (const auto &pathUtf8 : paths)
        {
            fs::path p = io::fromUtf8(pathUtf8);
            if (isReservedSegment(p.filename()))
                continue;

            try
            {
                if (fs::is_directory(p))
                {
                    walkDataset(p, options, [&](const fs::directory_entry &entry)
                    {
                        if (entry.is_directory() ? includeDirs : includeFiles)
                            result.push_back(entry.path());
                    });
                }
                else if (fs::exists(p) && includeFiles)
                {
                    // File
                    result.push_back(p);
                }
                else
                {
                    throw FSException("Path does not exist: " + io::toUtf8(p));
                }
            }
            catch (const fs::filesystem_error &e)
            {
                throw FSException(e.what());
            }
        }

        return result;
    }

    std::vector<std::string> expandPathList(const std::vector<std::string> &paths,
                                            bool recursive, int maxRecursionDepth)
    {
        std::vector<std::string> result;

        if (!recursive)
        {
            for (auto &p : paths)
            {
                result.push_back(io::toUtf8(fs::absolute(io::fromUtf8(p))));
            }
        }
        else
        {
            auto pl = getPathList(paths, true, maxRecursionDepth);
            for (auto &p : pl)
            {
                result.push_back(io::toUtf8(fs::absolute(p)));
            }
        }

        return result;
    }

    bool containsDdbSegment(const fs::path &p)
    {
        for (const auto &part : p)
        {
            if (isReservedSegment(part))
                return true;
        }
        return false;
    }

    namespace
    {
        // Returns true if the pattern contains shell glob meta-characters.
        bool isGlobPattern(const std::string &p)
        {
            return p.find_first_of("*?[") != std::string::npos;
        }
    } // namespace

    std::vector<std::string> expandGlobPatterns(
        const std::vector<std::string> &patterns)
    {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        size_t totalMatches = 0;

        const auto pushUnique = [&](const fs::path &p) {
            if (containsDdbSegment(p))
                return;
            std::string abs = io::toUtf8(fs::absolute(p));
            if (seen.insert(abs).second)
                result.push_back(std::move(abs));
        };

        for (const std::string &pattern : patterns)
        {
            if (pattern.empty())
                throw InvalidArgsException("Empty pattern in input list");

            if (isGlobPattern(pattern))
            {
                // `**` triggers recursive globbing; everything else is single-level.
                const bool recursive =
                    pattern.find("**") != std::string::npos;
                std::vector<fs::path> matches;
                try
                {
                    matches = recursive ? glob::rglob(pattern)
                                        : glob::glob(pattern);

                    // p-ranav's `**` requires at least one directory segment,
                    // so `**/<sub>` would skip files at the cwd. To match the
                    // common shell expectation that `**` matches "zero or more
                    // directories", also run the non-recursive variant with
                    // the leading `**/` stripped and merge the results.
                    if (recursive && pattern.starts_with("**/"))
                    {
                        const std::string topLevel = pattern.substr(3);
                        if (!topLevel.empty())
                        {
                            auto extra = glob::glob(topLevel);
                            matches.insert(matches.end(),
                                           std::make_move_iterator(extra.begin()),
                                           std::make_move_iterator(extra.end()));
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    LOGD << "Glob error for pattern '" << pattern
                         << "': " << e.what();
                }

                size_t patternMatches = 0;
                for (auto &m : matches)
                {
                    if (containsDdbSegment(m))
                        continue;
                    pushUnique(m);
                    ++patternMatches;
                }

                if (patternMatches == 0)
                {
                    LOGV << "Pattern '" << pattern
                         << "' did not match any files";
                }
                totalMatches += patternMatches;
            }
            else
            {
                fs::path p = io::fromUtf8(pattern);
                if (!fs::exists(p))
                    throw FSException("Path does not exist: " + pattern);

                if (fs::is_directory(p))
                {
                    // Bare directories: expand recursively (preserves the
                    // common `ddb add .` workflow). Hint at glob equivalent.
                    LOGV << "Expanding directory '" << pattern
                         << "' recursively (use '" << pattern
                         << "/**/*' for explicit globbing).";

                    const size_t before = result.size();
                    pushUnique(p);
                    walkDataset(p, {}, [&](const fs::directory_entry &entry) { pushUnique(entry.path()); });
                    // Only count this directory as a match if at least one
                    // entry was actually added (e.g. the directory wasn't
                    // skipped for being inside a `.ddb` folder).
                    if (result.size() > before)
                        ++totalMatches;
                }
                else
                {
                    const size_t before = result.size();
                    pushUnique(p);
                    if (result.size() > before)
                        ++totalMatches;
                }
            }
        }

        if (totalMatches == 0 && !patterns.empty())
            throw FSException(
                "No files matched any of the provided patterns");

        return result;
    }

} // namespace ddb
