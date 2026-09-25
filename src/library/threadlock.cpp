/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "threadlock.h"

namespace ddb
{

    struct KeyedMutex
    {
        std::mutex mutex;
        int refCount = 0;
    };

    namespace
    {
        // Guards the registry map only; never held while waiting on a per-key mutex.
        std::mutex registryMutex;
        std::unordered_map<std::string, std::shared_ptr<KeyedMutex>> registry;
    }

    ThreadLock::ThreadLock(const std::string &key) : key(key)
    {
        {
            std::lock_guard<std::mutex> guard(registryMutex);
            auto &slot = registry[key];
            if (!slot) slot = std::make_shared<KeyedMutex>();
            slot->refCount++;
            entry = slot;
        }

        entry->mutex.lock();
    }

    ThreadLock::~ThreadLock()
    {
        entry->mutex.unlock();

        std::lock_guard<std::mutex> guard(registryMutex);
        if (--entry->refCount <= 0)
        {
            registry.erase(key);
        }
    }

}
