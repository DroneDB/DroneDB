/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "ne_functions.h"
#include "ne_dbops.h"
#include "ne_share.h"
#include "ne_login.h"
#include "ddb.h"

using v8::FunctionTemplate;

// NativeExtension.cc represents the top level of the module.
// C++ constructs that are exposed to javascript are exported here

NAN_MODULE_INIT(InitAll) {
    NAN_EXPORT(target, getVersion);
    NAN_EXPORT(target, getDefaultRegistry);
	NAN_EXPORT(target, typeToHuman);
    NAN_EXPORT(target, info);
	NAN_EXPORT(target, _thumbs_getFromUserCache);
    NAN_EXPORT(target, _tile_getFromUserCache);
    NAN_EXPORT(target, init);
    NAN_EXPORT(target, add);
    NAN_EXPORT(target, remove);
    NAN_EXPORT(target, share);
    NAN_EXPORT(target, list);
    NAN_EXPORT(target, login);

	DDBRegisterProcess();
}

NODE_MODULE(ddb, InitAll)
