/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>

#include "log_forwarding.h"

// The build defines PLOG_CHAR_IS_UTF8=1 (src/CMakeLists.txt): a record keeps the UTF-8 bytes it
// was given (record.getMessage() is UTF-8), and plog converts them as UTF-8 for the Windows
// console (WriteConsoleW)
static_assert(PLOG_CHAR_IS_UTF8, "plog must keep log records in UTF-8 (mio.h, \"Path encoding\")");

namespace ddb::logging
{
namespace
{
    // The level of DDBLogLevel (log_forwarding.h) of a plog severity: verbose 0 .. fatal 5
    int levelOf(plog::Severity severity)
    {
        return plog::verbose - std::clamp(severity, plog::fatal, plog::verbose);
    }

    // The console, or the host's log callback while one is registered (DQ-15, XI-08): the
    // records go to the callback instead of the console. The console keeps its own level (info,
    // or verbose for a verbose process): the logger may let more through for the callback.
    // The console is stderr: stdout carries the output of the commands (ddb info -f json,
    // ddb search, ddb stamp, ...), which scripts parse, and a log record there breaks it.
    class HostAppender : public plog::IAppender
    {
    public:
        void write(const plog::Record &record) override
        {
            if (ddb::logfwd::deliver(levelOf(record.getSeverity()), "ddb", record.getMessage()))
                return;
            if (record.getSeverity() <= consoleMax_.load(std::memory_order_relaxed))
                console_.write(record);
        }

        void setConsoleMax(plog::Severity severity) { consoleMax_.store(severity); }
        plog::Severity consoleMax() const { return consoleMax_.load(); }

    private:
        plog::ConsoleAppender<plog::TxtFormatter> console_{plog::streamStdErr};
        std::atomic<plog::Severity> consoleMax_{plog::info};
    };

    HostAppender &hostAppender()
    {
        static HostAppender appender;
        return appender;
    }

    // What the logger lets through: the console's level, or more for a callback registered
    // before the initialization. Set at the initialization only: plog reads it unsynchronized.
    plog::Severity loggerSeverity(plog::Severity console)
    {
        const int minLevel = ddb::logfwd::callbackMinLevel();
        if (minLevel < 0)
            return console;
        return std::max(console, static_cast<plog::Severity>(plog::verbose - minLevel));
    }

    constexpr const char *kLogFileName = "ddb-log.csv";
}

void init(bool logToFile) {
    auto &host = hostAppender();
    host.setConsoleMax(plog::info);
    const plog::Severity severity = loggerSeverity(plog::info);

    if (logToFile) {
        static plog::RollingFileAppender<plog::CsvFormatter> fileAppender(kLogFileName, 32000, 5);
        plog::init(severity, &host).addAppender(&fileAppender);
    } else
        plog::init(severity, &host);
}

void setVerbose() {
    hostAppender().setConsoleMax(plog::verbose);
    plog::get()->setMaxSeverity(plog::verbose);
}

bool isVerbose() {
    return hostAppender().consoleMax() == plog::verbose;
}

void flush() {
    // The console appender writes to std::cerr, the commands their output to std::cout; GDAL's
    // default handler and the C stdio of the other libraries to stderr and stdout
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
}

}  // namespace ddb::logging
