/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "nexus_runner.h"

#include <string>
#include <system_error>
#include <vector>

#include "build_budget.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "tool_locator.h"
#include "tool_runner_result.h"

namespace ddb
{
    namespace nexus
    {

        namespace
        {

            constexpr const char *kNexusEnv = "DDB_NEXUS_PATH";

            // Tail of the tool's output in the error message. ddb-nexus ends its output with
            // the reason it failed, after the progress lines.
            constexpr size_t kErrorTailLength = 2000;

            [[noreturn]] void fail(const fs::path &input, const std::string &reason)
            {
                throw AppException("Could not build nexus file for " + io::toUtf8(input) + ": " + reason);
            }

        } // namespace

        fs::path findNexusBinary(bool forceRefresh)
        {
            return tools::findTool("ddb-nexus", kNexusEnv, forceRefresh);
        }

        void runNexusBuild(const fs::path &input,
                           const fs::path &output,
                           const fs::path &binary,
                           const process::IProcessRunner &runner,
                           const BuildOptions &options)
        {
            const fs::path bin = binary.empty() ? findNexusBinary() : binary;
            if (!tools::isExecutableFile(bin))
                fail(input, std::string("ddb-nexus not found; set ") + kNexusEnv +
                                " or install ddb-nexus next to ddb");

            // The threads of one build (DDB_BUILD_THREADS, as for the raster builds, lowered to the
            // caller's cap) that the CPU pool has free: the child keeps that many busy, and the
            // tokens are held until it ends
            const auto cpu =
                parallel::tryAcquireThreads(static_cast<size_t>(BuildResourceBudget::fromConfig(options).threads));
            const std::vector<std::string> args = {"--threads", std::to_string(cpu.threads()), io::toUtf8(input),
                                                   io::toUtf8(output)};
            LOGD << "Spawning ddb-nexus: " << process::formatCommandLine(bin, args);

            const process::Result res = runner.run(bin, args, process::Options());
            if (const auto failure = ToolRunResult::fromProcess("ddb-nexus", res, kErrorTailLength);
                !failure.succeeded)
                fail(input, failure.error);

            std::error_code ec;
            if (!fs::is_regular_file(output, ec) || ec || fs::file_size(output, ec) == 0 || ec)
                fail(input, "ddb-nexus reported success but wrote no output file " + io::toUtf8(output) +
                                (res.outputTail.empty() ? std::string() : ": " + res.tail(kErrorTailLength)));

            LOGD << "Built nexus file " << io::toUtf8(output) << " from " << io::toUtf8(input) << " in "
                 << (static_cast<double>(res.duration.count()) / 1000.0) << " s";
        }

    } // namespace nexus
} // namespace ddb
