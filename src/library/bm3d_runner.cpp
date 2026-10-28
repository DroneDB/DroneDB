/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "bm3d_runner.h"

#include <string>
#include <system_error>
#include <vector>

#include "build_budget.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "tool_locator.h"
#include "tool_run_support.h"

namespace ddb {
namespace bm3d {

namespace {

// Tail of the tool's output in the error message. bm3d ends its output with the
// reason it failed, after the progress lines.
constexpr size_t kErrorTailLength = 2000;

std::string failureMessage(const fs::path& input, const std::string& reason) {
    return "Could not build bm3d file for " + io::toUtf8(input) + ": " + reason;
}

[[noreturn]] void fail(const fs::path& input, const std::string& reason) {
    throw AppException(failureMessage(input, reason));
}

}  // namespace

fs::path findBm3dBinary(bool forceRefresh) {
    return tools::findTool(kBm3dName, kBm3dEnv, forceRefresh);
}

void runBm3dBuild(const fs::path& input,
                  const fs::path& output,
                  const fs::path& binary,
                  const process::IProcessRunner& runner,
                  const BuildOptions& options) {
    const fs::path bin = binary.empty() ? findBm3dBinary() : binary;
    if (!tools::isExecutableFile(bin))
        fail(input,
             std::string(kBm3dName) + " not found; set " + kBm3dEnv + " or install " + kBm3dName +
                 " next to ddb");

    // The threads of one build (DDB_BUILD_THREADS, as for the raster builds, lowered to the
    // caller's cap) that the CPU pool has free: the child keeps that many busy, and the
    // tokens are held until it ends
    const auto cpu = parallel::tryAcquireThreads(
        static_cast<size_t>(BuildResourceBudget::fromConfig(options).threads));
    const std::vector<std::string> args = {"--threads",
                                           std::to_string(cpu.threads()),
                                           io::toUtf8(input),
                                           io::toUtf8(output)};
    LOGD << "Spawning bm3d: " << process::formatCommandLine(bin, args);

    const auto run = tools::runToolProducingFile(
        {kBm3dName, bin, args, process::Options(), output, kErrorTailLength, "wrote no output file"}, runner);
    const process::Result &res = run.process;
    if (!run.result.succeeded) {
        // Exit code 2: bm3d read the command line and refused the model itself
        if (res.exited && res.exitCode == kExitInvalidInput)
            throw InvalidArgsException(failureMessage(input, run.result.error));
        fail(input, run.result.error);
    }

    LOGD << "Built bm3d file " << io::toUtf8(output) << " from " << io::toUtf8(input) << " in "
         << (static_cast<double>(res.duration.count()) / 1000.0) << " s";
}

}  // namespace bm3d
}  // namespace ddb
