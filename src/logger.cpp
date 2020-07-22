/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "logger.h"

void init_logger() {
	static plog::ConsoleAppender<plog::TxtFormatter> consoleAppender;
    plog::init(plog::info, &consoleAppender);
}

void set_logger_verbose() {
    plog::get()->setMaxSeverity(plog::verbose);
}

bool is_logger_verbose(){
    return plog::get()->getMaxSeverity() == plog::verbose;
}
