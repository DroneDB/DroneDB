/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TOOL_RUN_SUPPORT_H
#define TOOL_RUN_SUPPORT_H

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "fs.h"
#include "process_runner.h"
#include "tool_runner_result.h"

namespace ddb
{
    namespace tools
    {
        // A launch of an external tool that is expected to write one file.
        struct ToolRun
        {
            std::string tool;              // the name in the error messages ("build-lod")
            fs::path binary;               // an executable the caller has located
            std::vector<std::string> args;
            process::Options options;
            fs::path output;               // the file the tool writes when it succeeds
            size_t tailLength = 600;       // bytes of the tool's output kept in an error message
            // What the error says about a missing or empty output, after "<tool> reported
            // success but ": a tool that exits 0 without writing is told apart from one that
            // fails, and the message carries what the tool printed.
            std::string missingOutputText = "expected output is missing";
        };

        struct ToolRunOutcome
        {
            ToolRunResult result;
            process::Result process; // how the child ended (the caller classifies exit codes)
        };

        // Starts the tool and checks what it left: the failure of the child ("<tool> exited with
        // code 3: <last output>", the timeout flag included), or an exit 0 without a non-empty
        // regular file at run.output, which many tools do after a decode failure they only print.
        // Both carry the tail of the tool's output, because that is where the tool says why.
        // Moving the file, classifying exit codes and retrying stay with the caller.
        ToolRunOutcome runToolProducingFile(const ToolRun &run, const process::IProcessRunner &runner);

        // How long a run of a tool may take in total: the whole seconds in the environment
        // variable envName (1 and up), or defaultValue when it is unset or invalid (the warning
        // is logged). The value for process::Options::maxRuntime.
        std::chrono::milliseconds toolMaxRuntime(const char *envName, std::chrono::seconds defaultValue);

        // A tool's output tail as one display line: newlines, carriage returns and tabs become
        // spaces.
        std::string oneLine(std::string_view text);

    } // namespace tools
} // namespace ddb

#endif // TOOL_RUN_SUPPORT_H
