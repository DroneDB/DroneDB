/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>

#include "log_forwarding.h"

namespace
{
    // A record's message as UTF-8: plog keeps it in UTF-8 (char) except on Windows without
    // /utf-8, where it converted the narrow input with the active code page into wchar_t
#if PLOG_CHAR_IS_UTF8
    const char *messageText(const plog::Record &record, std::string &)
    {
        return record.getMessage();
    }
#else
    const char *messageText(const plog::Record &record, std::string &buffer)
    {
        buffer = plog::util::toNarrow(record.getMessage(), plog::codePage::kChar);
        return buffer.c_str();
    }
#endif

    // The level of DDBLogLevel (log_forwarding.h) of a plog severity: verbose 0 .. fatal 5
    int levelOf(plog::Severity severity)
    {
        return plog::verbose - std::clamp(severity, plog::fatal, plog::verbose);
    }

    // The console, or the host's log callback while one is registered (DQ-15, XI-08): the
    // records go to the callback instead of stdout. The console keeps its own level (info, or
    // verbose for a verbose process): the logger may let more through for the callback.
    class HostAppender : public plog::IAppender
    {
    public:
        void write(const plog::Record &record) override
        {
            std::string buffer;
            if (ddb::logfwd::deliver(levelOf(record.getSeverity()), "ddb", messageText(record, buffer)))
                return;
            if (record.getSeverity() <= consoleMax_.load(std::memory_order_relaxed))
                console_.write(record);
        }

        void setConsoleMax(plog::Severity severity) { consoleMax_.store(severity); }
        plog::Severity consoleMax() const { return consoleMax_.load(); }

    private:
        plog::ConsoleAppender<plog::TxtFormatter> console_;
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
}

void init_logger(bool logToFile) {
    auto &host = hostAppender();
    host.setConsoleMax(plog::info);
    const plog::Severity severity = loggerSeverity(plog::info);

    if (logToFile) {
        static plog::RollingFileAppender<plog::CsvFormatter> fileAppender(LOG_FILE_NAME, 32000, 5);
        plog::init(severity, &host).addAppender(&fileAppender);
    } else
        plog::init(severity, &host);
}

void set_logger_verbose() {
    hostAppender().setConsoleMax(plog::verbose);
    plog::get()->setMaxSeverity(plog::verbose);
}

bool is_logger_verbose(){
    return hostAppender().consoleMax() == plog::verbose;
}

void flush_logger() {
    // The console appender writes to std::cout; GDAL's default handler and the C stdio of the
    // other libraries to stderr and stdout
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
}
