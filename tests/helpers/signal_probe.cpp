/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DDB-11 B6 probe. Acts like a host process (the .NET Registry): it owns its crash and
// termination handlers, then calls libddb's entry point. Exits 0 when
// DDBRegisterProcess left every handler in place, 1 (and names them) otherwise.
// Runs as its own process because DDBRegisterProcess initializes once per process.

#include <iostream>

#include "ddb.h"

#ifdef _WIN32
#include <windows.h>

static LONG WINAPI sentinelFilter(EXCEPTION_POINTERS *) { return EXCEPTION_CONTINUE_SEARCH; }

int main()
{
    SetUnhandledExceptionFilter(sentinelFilter);
    DDBRegisterProcess(false);
    if (SetUnhandledExceptionFilter(nullptr) != sentinelFilter)
    {
        std::cerr << "unhandled exception filter replaced" << std::endl;
        return 1;
    }
    return 0;
}

#else
#include <csignal>
#include <cstring>

static void sentinel(int, siginfo_t *, void *) {}

int main()
{
    const int signals[] = {SIGSEGV, SIGFPE, SIGTERM, SIGINT};

    struct sigaction sa {};
    sa.sa_sigaction = sentinel;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (const int sig : signals)
        sigaction(sig, &sa, nullptr);

    DDBRegisterProcess(false);

    int replaced = 0;
    for (const int sig : signals)
    {
        struct sigaction current {};
        sigaction(sig, nullptr, &current);
        if (!(current.sa_flags & SA_SIGINFO) || current.sa_sigaction != sentinel)
        {
            std::cerr << strsignal(sig) << " handler replaced" << std::endl;
            replaced++;
        }
    }
    return replaced == 0 ? 0 : 1;
}
#endif
