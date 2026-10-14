/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TOOL_RUNNER_RESULT_H
#define TOOL_RUNNER_RESULT_H

#include <string>
#include <system_error>
#include "exceptions.h"

namespace ddb
{
    // Internal structured result behind the public bool/error-string APIs.
    struct ToolRunResult
    {
        bool succeeded = false;
        bool timedOut = false;
        std::error_code errorCode;
        std::string error;

        void throwIfRetryable(const std::string &context) const
        {
            if (timedOut)
                throw RetryableBuildException(context + error, RetryableBuildReason::IdleTimeout);
            if (errorCode == std::errc::no_space_on_device)
                throw RetryableBuildException(context + error, RetryableBuildReason::NoSpaceOnDevice);
        }
    };
}
#endif
