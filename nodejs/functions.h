/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include <nan.h>

// Top-level functions

NAN_METHOD(getVersion);
NAN_METHOD(typeToHuman);
NAN_METHOD(parseFiles);
NAN_METHOD(_thumbs_getFromUserCache);
NAN_METHOD(_tile_getFromUserCache);



#endif
