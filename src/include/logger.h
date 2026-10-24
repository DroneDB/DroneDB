/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef LOGGER_H
#define LOGGER_H

#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Initializers/RollingFileInitializer.h>
#include <plog/Appenders/ConsoleAppender.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Formatters/CsvFormatter.h>
#include <plog/Formatters/MessageOnlyFormatter.h>


namespace ddb::logging {

// Starts the logger (once per process): the console, or the host's log callback, and with
// logToFile also the rolling file ddb-log.csv
void init(bool logToFile = false);
void setVerbose();
bool isVerbose();
// Writes out what the log appenders buffered (stdout, stderr): before the host exits (DDBShutdown)
void flush();

}  // namespace ddb::logging

#endif // LOGGER_H
