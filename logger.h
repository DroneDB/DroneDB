/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#ifndef LOGGER_H
#define LOGGER_H

#include "libs/plog/Log.h"
#include "libs/plog/Appenders/ConsoleAppender.h"
#include "libs/plog/Formatters/MessageOnlyFormatter.h"

#define LOGV PLOG_VERBOSE
#define LOGD PLOG_DEBUG
#define LOGI PLOG_INFO
#define LOGW PLOG_WARNING
#define LOGE PLOG_ERROR
#define LOGF PLOG_FATAL
#define LOGN PLOG_NONE

void init_logger();
void set_logger_verbose();

#endif // LOGGER_H
