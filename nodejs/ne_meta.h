/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NE_META_H
#define NE_META_H

#include <nan.h>

NAN_METHOD(metaAdd);
NAN_METHOD(metaSet);
NAN_METHOD(metaRemove);
NAN_METHOD(metaGet);
NAN_METHOD(metaUnset);
NAN_METHOD(metaList);

#endif
