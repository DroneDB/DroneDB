/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tool_run_support.h"

#include <cstdint>
#include <system_error>

#include "mio.h"

namespace ddb
{
    namespace tools
    {

        ToolRunOutcome runToolProducingFile(const ToolRun &run, const process::IProcessRunner &runner)
        {
            ToolRunOutcome outcome;
            outcome.process = runner.run(run.binary, run.args, run.options);
            outcome.result = ToolRunResult::fromProcess(run.tool, outcome.process, run.tailLength);
            if (!outcome.result.succeeded)
                return outcome;

            std::error_code ec;
            const bool regular = fs::is_regular_file(run.output, ec) && !ec;
            const auto size = regular ? fs::file_size(run.output, ec) : std::uintmax_t{0};
            if (regular && !ec && size > 0)
                return outcome;

            ToolRunResult failure;
            failure.error = run.tool + " reported success but " + run.missingOutputText + ": " + io::toUtf8(run.output);
            if (regular && !ec)
                failure.error += " (the file is empty)";
            if (!outcome.process.outputTail.empty())
                failure.error += ": " + outcome.process.tail(run.tailLength);
            outcome.result = failure;
            return outcome;
        }

    } // namespace tools
} // namespace ddb
