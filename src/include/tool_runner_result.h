/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TOOL_RUNNER_RESULT_H
#define TOOL_RUNNER_RESULT_H

#include <string>
#include <string_view>
#include <system_error>
#include "exceptions.h"
#include "process_runner.h"

namespace ddb
{
    // Internal structured result behind the public bool/error-string APIs.
    struct ToolRunResult
    {
        bool succeeded = false;
        bool timedOut = false;
        std::error_code errorCode;
        std::string error;

        // The outcome of a supervised child: success, or the failure with the text "<tool>
        // exited with code 3: <last output>" and the timeout flag the retry policy reads.
        // The checks a runner makes on what the tool produced come after this one.
        static ToolRunResult fromProcess(std::string_view tool, const process::Result &res, size_t tailLength)
        {
            ToolRunResult result;
            if (res.success())
            {
                result.succeeded = true;
                return result;
            }
            result.timedOut = res.timedOut;
            result.error = std::string(tool) + " " + res.failureText(tailLength);
            return result;
        }

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
