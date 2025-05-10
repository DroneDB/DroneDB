/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "threadlock.h"

namespace ddb
{

    std::unordered_map<std::string, std::mutex> _mutexes;
    std::unordered_map<std::string, int> _mutexesCount;

    ThreadLock::ThreadLock(const std::string &key) : key(key)
    {
        if (_mutexesCount.find(key) == _mutexesCount.end())
        {
            _mutexesCount[key] = 1;
        }
        else
        {
            _mutexesCount[key]++;
        }

        (_mutexes[key]).lock();
    }

    ThreadLock::~ThreadLock()
    {
        (_mutexes[key]).unlock();

        if (--_mutexesCount[key] <= 0)
        {
            _mutexes.erase(key);
            _mutexesCount.erase(key);
        }
    }

}
