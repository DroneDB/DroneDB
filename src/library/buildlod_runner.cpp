/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "buildlod_runner.h"

#include <chrono>
#include <string>
#include <system_error>
#include <vector>

#include "env.h"
#include "logger.h"
#include "mio.h"
#include "process_runner.h"
#include "tool_locator.h"

namespace ddb
{
    namespace buildlod
    {

        namespace
        {

            // Tail of the captured output included in errorOut.
            constexpr size_t kErrorTailLength = 512;

            // build-lod derives its output name from the input: it replaces the input's
            // extension with "-lod" and appends ".rad" (see rust/build-lod/src/main.rs).
            // For "<dir>/model.spz" the produced file is "<dir>/model-lod.rad".
            fs::path expectedLodOutput(const fs::path &input)
            {
                fs::path stem = input.stem(); // "model.spz" -> "model"
                return input.parent_path() / io::concat(stem, "-lod.rad");
            }

        } // namespace

        fs::path findBuildLodBinary(bool forceRefresh)
        {
            return tools::findTool("build-lod", "DDB_BUILDLOD_PATH", forceRefresh);
        }

        bool isBuildLodAvailable()
        {
            return !findBuildLodBinary().empty();
        }

        bool runBuildLod(const fs::path &input,
                         const fs::path &outputRad,
                         std::string &errorOut,
                         bool quality,
                         int maxSh,
                         const fs::path &binary)
        {
            const auto result = runBuildLodResult({input, outputRad, quality, maxSh, binary});
            errorOut = result.error;
            return result.succeeded;
        }

        ToolRunResult runBuildLodResult(const BuildLodRequest &request,
                                       const process::Options &options,
                                       const process::IProcessRunner &runner)
        {
            const fs::path &input = request.input;
            const fs::path &outputRad = request.outputRad;
            const bool quality = request.quality;
            const int maxSh = request.maxSh;
            const fs::path &binary = request.binary;
            ToolRunResult result;
            process::Options popts = options;
            // 24 hours: keeps steady-clock deadlines representable
            const auto idle = env::readPositiveInt("DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS", 86400);
            if (idle.state == env::Integer::State::Invalid)
            {
                result.error = "Invalid DDB_BUILDLOD_IDLE_TIMEOUT_SECONDS; expected an integer from 1 to 86400 seconds (24 hours)";
                return result;
            }
            if (idle.state == env::Integer::State::Valid)
                popts.idleTimeout = std::chrono::seconds(static_cast<std::chrono::seconds::rep>(idle.value));
            const fs::path bin = binary.empty() ? findBuildLodBinary() : binary;
            if (!tools::isExecutableFile(bin))
            {
                result.error = "build-lod binary not found";
                return result;
            }

            std::error_code ec;
            if (!fs::exists(input, ec) || ec)
            {
                result.errorCode = ec;
                result.error = "build-lod input does not exist: " + io::toUtf8(input);
                return result;
            }

            const fs::path producedRad = expectedLodOutput(input);
            // Preserve the actual error before cleanup can overwrite it.
            auto failIo = [&result](const std::string &operation, const std::error_code &code) {
                result.errorCode = code;
                result.error = operation + ": " + code.message();
                return result;
            };
            fs::remove(producedRad, ec);
            if (ec) return failIo("Cannot remove stale build-lod output", ec);
            fs::remove(outputRad, ec);
            if (ec) return failIo("Cannot remove stale RAD output", ec);
            if (!outputRad.parent_path().empty()) {
                fs::create_directories(outputRad.parent_path(), ec);
                if (ec) return failIo("Cannot create RAD output directory", ec);
            }

            int clampedSh = maxSh < 0 ? 0 : (maxSh > 3 ? 3 : maxSh);
            std::vector<std::string> args = {io::toUtf8(input), quality ? "--quality" : "--quick",
                                            "--max-sh=" + std::to_string(clampedSh), "--rad"};
            LOGD << "Spawning build-lod: " << process::formatCommandLine(bin, args);

            popts.onOutputLine = [](const std::string &line) { LOGD << "[build-lod] " << line; };
            const process::Result res = runner.run(bin, args, popts);
            if (auto failure = ToolRunResult::fromProcess("build-lod", res, kErrorTailLength); !failure.succeeded)
            {
                fs::remove(producedRad, ec);
                return failure;
            }

            if (!fs::exists(producedRad, ec) || ec)
            {
                result.errorCode = ec;
                result.error = "build-lod reported success but expected output is missing: " + io::toUtf8(producedRad);
                return result;
            }

            fs::rename(producedRad, outputRad, ec);
            if (ec)
            {
                // Cross-device moves may succeed via copy. An identifiable full disk
                // is terminal for this attempt rather than an unexplained tool failure.
                if (ec == std::errc::no_space_on_device)
                    return failIo("Cannot move build-lod output", ec);
                std::error_code copyEc;
                fs::copy_file(producedRad, outputRad, fs::copy_options::overwrite_existing, copyEc);
                if (copyEc)
                    return failIo("failed to move build-lod output to " + io::toUtf8(outputRad), copyEc);
                fs::remove(producedRad, ec);
            }
            result.succeeded = true;
            return result;
        }

    } // namespace buildlod
} // namespace ddb
