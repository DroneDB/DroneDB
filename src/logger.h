/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef LOGGER_H
#define LOGGER_H

#include "../vendor/plog/Log.h"
#include "../vendor/plog/Init.h"
#include "../vendor/plog/Appenders/ConsoleAppender.h"
#include "../vendor/plog/Appenders/RollingFileAppender.h"
#include "../vendor/plog/Formatters/TxtFormatter.h"
#include "../vendor/plog/Formatters/CsvFormatter.h"
#include "../vendor/plog/Formatters/MessageOnlyFormatter.h"
#include "ddb_export.h"

#define LOG_FILE_NAME "ddb-log.csv"

DDB_DLL void init_logger(bool logToFile = false);
DDB_DLL void set_logger_verbose();
DDB_DLL bool is_logger_verbose();

#endif // LOGGER_H
