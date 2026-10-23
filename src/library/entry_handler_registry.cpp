/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The entry handler registry (entry_handler.h) and the handlers of the types without
// capabilities of their own (Undefined, Directory, Generic, DroneDB, Markdown).

#include "entry_handler.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "entry_handlers_builtin.h"
#include "utils.h"
#include "mio.h"

namespace ddb {

bool fileExistsAndNonEmpty(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec)
        return false;
    if (!fs::is_regular_file(p, ec) || ec)
        return false;
    const auto sz = fs::file_size(p, ec);
    if (ec)
        return false;
    return sz > 0;
}

bool directoryHasNonEmptyContent(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec)
        return false;
    if (!fs::is_directory(dir, ec) || ec)
        return false;

    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        std::error_code fec;
        if (entry.is_regular_file(fec) && !fec) {
            const auto sz = entry.file_size(fec);
            if (!fec && sz > 0)
                return true;
        }
    }

    return false;
}

namespace {

// A type known by its name (and extensions) only
class PlainEntryHandler : public EntryHandler {
  public:
    PlainEntryHandler(EntryType type, std::string name, std::vector<std::string> extensions = {})
        : EntryHandler(type, std::move(name)), extensions_(std::move(extensions)) {}

    std::vector<std::string> extensions() const override { return extensions_; }

  private:
    std::vector<std::string> extensions_;
};

class Registry {
  public:
    Registry() { registerBuiltins(); }

    const EntryHandler* find(EntryType type) const {
        std::shared_lock lock(mutex_);
        const auto it = handlers_.find(type);
        return it != handlers_.end() ? it->second.get() : nullptr;
    }

    const EntryHandler* findByExtension(const std::string& ext) const {
        std::shared_lock lock(mutex_);
        const auto it = byExtension_.find(ext);
        return it != byExtension_.end() ? it->second : nullptr;
    }

    std::vector<const EntryHandler*> all() const {
        std::shared_lock lock(mutex_);
        std::vector<const EntryHandler*> result;
        result.reserve(handlers_.size());
        for (const auto& [type, handler] : handlers_)
            result.push_back(handler.get());
        return result;
    }

    std::unique_ptr<EntryHandler> add(std::unique_ptr<EntryHandler> handler) {
        if (!handler)
            throw InvalidArgsException("Cannot register a null entry handler");

        std::unique_lock lock(mutex_);
        const EntryType type = handler->type();
        for (const auto& ext : handler->extensions()) {
            const auto it = byExtension_.find(ext);
            if (it != byExtension_.end() && it->second->type() != type)
                throw InvalidArgsException("Extension ." + ext + " of " + handler->name() +
                                           " already belongs to " + it->second->name());
        }

        std::unique_ptr<EntryHandler> previous;
        if (const auto it = handlers_.find(type); it != handlers_.end()) {
            previous = std::move(it->second);
            handlers_.erase(it);
        }
        handlers_.emplace(type, std::move(handler));
        reindexExtensions();
        return previous;
    }

    std::unique_ptr<EntryHandler> remove(EntryType type) {
        std::unique_lock lock(mutex_);
        const auto it = handlers_.find(type);
        if (it == handlers_.end())
            return nullptr;
        auto previous = std::move(it->second);
        handlers_.erase(it);
        reindexExtensions();
        return previous;
    }

  private:
    // The built-in types. A new type adds its handler here (one line) and nothing else.
    void registerBuiltins() {
        add(std::make_unique<PlainEntryHandler>(EntryType::Undefined, "Undefined"));
        add(std::make_unique<PlainEntryHandler>(EntryType::Directory, "Directory"));
        add(std::make_unique<PlainEntryHandler>(EntryType::Generic, "Generic"));
        add(makeImageEntryHandler(EntryType::GeoImage));
        add(makeGeoRasterEntryHandler());
        add(makePointCloudEntryHandler());
        add(makeImageEntryHandler(EntryType::Image));
        add(std::make_unique<PlainEntryHandler>(EntryType::DroneDB, "DroneDB"));
        add(std::make_unique<PlainEntryHandler>(EntryType::Markdown, "Markdown",
                                                std::vector<std::string>{"md"}));
        add(makeImageEntryHandler(EntryType::Video));
        add(makeImageEntryHandler(EntryType::GeoVideo));
        add(makeModelEntryHandler());
        add(makeImageEntryHandler(EntryType::Panorama));
        add(makeImageEntryHandler(EntryType::GeoPanorama));
        add(makeVectorEntryHandler());
        add(makeGaussianSplatEntryHandler());
        add(makeTiles3DEntryHandler());
    }

    // Called with the lock held
    void reindexExtensions() {
        byExtension_.clear();
        for (const auto& [type, handler] : handlers_)
            for (const auto& ext : handler->extensions())
                byExtension_.emplace(ext, handler.get());
    }

    mutable std::shared_mutex mutex_;
    std::map<EntryType, std::unique_ptr<EntryHandler>> handlers_;
    std::unordered_map<std::string, const EntryHandler*> byExtension_;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

}  // namespace

const EntryHandler* findEntryHandler(EntryType type) {
    return registry().find(type);
}

const EntryHandler& entryHandler(EntryType type) {
    if (const EntryHandler* handler = findEntryHandler(type))
        return *handler;
    static const PlainEntryHandler unknown(EntryType::Undefined, "?");
    return unknown;
}

std::vector<const EntryHandler*> entryHandlers() {
    return registry().all();
}

const EntryHandler* findEntryHandlerByExtension(const fs::path& path) {
    const std::string ext = io::lowerExtension(path);
    return ext.empty() ? nullptr : registry().findByExtension(ext);
}

std::vector<EntryType> buildableEntryTypes() {
    std::vector<EntryType> types;
    for (const EntryHandler* handler : entryHandlers())
        if (handler->buildable())
            types.push_back(handler->type());
    return types;
}

const ThumbnailGenerator* findThumbnailGenerator(const fs::path& input) {
    for (const EntryHandler* handler : entryHandlers()) {
        const ThumbnailGenerator* generator = handler->thumbnailGenerator();
        if (generator != nullptr && generator->accepts(input))
            return generator;
    }
    return nullptr;
}

const TilerFactory* findTilerFactory(const fs::path& tileablePath) {
    for (const EntryHandler* handler : entryHandlers()) {
        const TilerFactory* factory = handler->tilerFactory();
        if (factory != nullptr && factory->accepts(tileablePath))
            return factory;
    }
    return nullptr;
}

std::vector<std::string> sidecarMainExtensions(const std::string& path) {
    const std::string ext = io::lowerExtension(io::fromUtf8(path));
    std::vector<std::string> mains;
    if (ext.empty())
        return mains;

    for (const EntryHandler* handler : entryHandlers())
        for (const auto& rule : handler->sidecarRules()) {
            const auto& sidecars = rule.sidecarExtensions;
            if (std::find(sidecars.begin(), sidecars.end(), ext) == sidecars.end())
                continue;
            for (const auto& main : rule.mainExtensions)
                if (std::find(mains.begin(), mains.end(), main) == mains.end())
                    mains.push_back(main);
        }
    return mains;
}

std::unique_ptr<EntryHandler> registerEntryHandler(std::unique_ptr<EntryHandler> handler) {
    return registry().add(std::move(handler));
}

std::unique_ptr<EntryHandler> unregisterEntryHandler(EntryType type) {
    return registry().remove(type);
}

}  // namespace ddb
