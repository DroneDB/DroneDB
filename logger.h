/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef LOGGER_H
#define LOGGER_H

#include "vendor/plog/Log.h"
#include "vendor/plog/Appenders/ConsoleAppender.h"
#include "vendor/plog/Formatters/MessageOnlyFormatter.h"

void init_logger();
void set_logger_verbose();
bool is_logger_verbose();

#endif // LOGGER_H
