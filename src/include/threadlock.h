/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THREADLOCK_H
#define THREADLOCK_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>

namespace ddb
{

    struct KeyedMutex;

    class ThreadLock
    {
        std::string key;
        std::shared_ptr<KeyedMutex> entry;

    public:
        ThreadLock(const std::string &key);
        ~ThreadLock();
        ThreadLock(const ThreadLock &) = delete;
        ThreadLock &operator=(const ThreadLock &) = delete;
    };

}

#endif