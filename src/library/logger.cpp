/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "logger.h"

#include <cstdio>
#include <iostream>

void init_logger(bool logToFile) {
	static plog::ConsoleAppender<plog::TxtFormatter> consoleAppender;

    if (logToFile) {
        static plog::RollingFileAppender<plog::CsvFormatter> fileAppender(LOG_FILE_NAME, 32000, 5);
        plog::init(plog::info, &consoleAppender).addAppender(&fileAppender);
    } else
        plog::init(plog::info, &consoleAppender);
}

void set_logger_verbose() {
    plog::get()->setMaxSeverity(plog::verbose);
}

bool is_logger_verbose(){
    return plog::get()->getMaxSeverity() == plog::verbose;
}

void flush_logger() {
    // The console appender writes to std::cout; GDAL's default handler and the C stdio of the
    // other libraries to stderr and stdout
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
}
