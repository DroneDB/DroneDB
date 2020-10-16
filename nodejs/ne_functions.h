/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NE_FUNCTIONS_H
#define NE_FUNCTIONS_H

#include <nan.h>

NAN_METHOD(getVersion);
NAN_METHOD(getDefaultRegistry);
NAN_METHOD(info);
NAN_METHOD(_thumbs_getFromUserCache);
NAN_METHOD(_tile_getFromUserCache);

#endif
