/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "sanitizer_hooks.h"

#include <atomic>

namespace
{
    std::atomic<bool> leakCheckDisabled{false};
}

// LeakSanitizer calls this hook (a weak symbol in its runtime) before the exit-time check and
// skips the check when it returns non-zero. Unused in builds without LSan.
extern "C" int __lsan_is_turned_off()
{
    return leakCheckDisabled.load() ? 1 : 0;
}

void disableLeakCheckAtExit()
{
    leakCheckDisabled.store(true);
}
