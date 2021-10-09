/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef THREADLOCK_H
#define THREADLOCK_H

#include <mutex>
#include <unordered_map>

namespace ddb{

class ThreadLock {
    std::string key;
    public:
        ThreadLock(const std::string &key);
        ~ThreadLock();
};

}


#endif