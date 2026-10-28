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
#include "exceptions.h"
#include "mio.h"
#include "parallel.h"
#include "process_runner.h"
#include "tool_locator.h"
#include "tool_run_support.h"

namespace ddb
{
    namespace buildlod
    {

        namespace
        {

            // Tail of the captured output included in errorOut.
            constexpr size_t kErrorTailLength = 512;

            // build-lod derives its output name from the input file name: it replaces the
            // extension with "-lod" and appends ".rad" (see rust/build-lod/src/main.rs), and it
            // silently skips an input that already ends in "-lod.spz" or "-lod.rad". It is given a
            // file of a name that is ours, in a folder that is ours: "source.<ext>" in a scratch
            // folder yields "source-lod.rad" there. It never writes beside, or deletes in, the
            // dataset folder, whatever the dataset file is called.
            constexpr const char *kStagedStem = "source";
            constexpr const char *kStagedOutput = "source-lod.rad";

            // build-lod is one busy thread of its own (no thread option, no thread pool); the
            // calling thread only waits for it, so it counts as one more thread beside the others
            constexpr size_t kBuildLodThreads = 2;

            // Makes `staged` name the content of `input` without writing to the dataset: a hard
            // link, else a symbolic link (another volume), else a copy (a file system without
            // links, or Windows without the right to create them).
            void stageInput(const fs::path &input, const fs::path &staged)
            {
                std::error_code ec;
                fs::create_hard_link(input, staged, ec);
                if (!ec) return;
                LOGD << "Cannot hard link " << io::toUtf8(input) << " (" << ec.message() << "), trying a symbolic link";
                fs::create_symlink(fs::absolute(input), staged, ec);
                if (!ec) return;
                LOGD << "Cannot link " << io::toUtf8(input) << " (" << ec.message() << "), copying it";
                io::copy(input, staged);
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

            // Preserve the actual error before cleanup can overwrite it.
            auto failIo = [&result](const std::string &operation, const std::error_code &code) {
                result.errorCode = code;
                result.error = operation + ": " + code.message();
                return result;
            };
            fs::remove(outputRad, ec);
            if (ec) return failIo("Cannot remove stale RAD output", ec);
            if (!outputRad.parent_path().empty()) {
                fs::create_directories(outputRad.parent_path(), ec);
                if (ec) return failIo("Cannot create RAD output directory", ec);
            }

            // The input and what build-lod writes live in a scratch folder beside the output
            // (the build temp folder, which is published as is), removed on every path out
            io::ScopedTempDir scratch;
            const fs::path staged = [&]() -> fs::path {
                try
                {
                    const fs::path parent = outputRad.parent_path().empty() ? fs::path(".") : outputRad.parent_path();
                    scratch = io::ScopedTempDir::create(parent, ".build-lod-");
                    const fs::path file = scratch.path() / io::concat(fs::path(kStagedStem), io::toUtf8(input.extension()));
                    stageInput(input, file);
                    return file;
                }
                catch (const FSException &e)
                {
                    result.errorCode = e.code();
                    result.error = std::string("Cannot stage the build-lod input: ") + e.what();
                    return fs::path();
                }
            }();
            if (staged.empty()) return result;
            const fs::path producedRad = scratch.path() / kStagedOutput;

            int clampedSh = maxSh < 0 ? 0 : (maxSh > 3 ? 3 : maxSh);
            std::vector<std::string> args = {io::toUtf8(staged), quality ? "--quality" : "--quick",
                                            "--max-sh=" + std::to_string(clampedSh), "--rad"};
            LOGD << "Spawning build-lod: " << process::formatCommandLine(bin, args);

            const auto cpu = parallel::tryAcquireThreads(kBuildLodThreads);
            LOGD << "build-lod CPU tokens: " << cpu.count();

            popts.onOutputLine = [](const std::string &line) { LOGD << "[build-lod] " << line; };
            result = tools::runToolProducingFile({"build-lod", bin, args, popts, producedRad, kErrorTailLength}, runner).result;
            if (!result.succeeded) return result;

            try
            {
                io::moveOrCopy(producedRad, outputRad);
            }
            catch (const FSException &e)
            {
                // A full disk is terminal for this attempt (the code says so) rather than an
                // unexplained tool failure
                fs::remove(outputRad, ec);
                result = ToolRunResult();
                result.errorCode = e.code();
                result.error = "Cannot move build-lod output to " + io::toUtf8(outputRad) + ": " + e.what();
                return result;
            }
            result.succeeded = true;
            return result;
        }

    } // namespace buildlod
} // namespace ddb
