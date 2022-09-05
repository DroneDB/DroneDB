/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "ne_functions.h"
#include "ne_dbops.h"
#include "ne_share.h"
#include "ne_login.h"
#include "ne_meta.h"
#include "ne_shell.h"
#include "ddb.h"

using v8::FunctionTemplate;

// NativeExtension.cc represents the top level of the module.
// C++ constructs that are exposed to javascript are exported here

NAN_MODULE_INIT(InitAll) {
    NAN_EXPORT(target, getVersion);
    NAN_EXPORT(target, getDefaultRegistry);
    NAN_EXPORT(target, info);
	NAN_EXPORT(target, _thumbs_getFromUserCache);
    NAN_EXPORT(target, _tile_getFromUserCache);
    NAN_EXPORT(target, init);
    NAN_EXPORT(target, add);
    NAN_EXPORT(target, remove);
    NAN_EXPORT(target, move);
    NAN_EXPORT(target, share);
    NAN_EXPORT(target, list);
    NAN_EXPORT(target, build);
    NAN_EXPORT(target, login);
    NAN_EXPORT(target, search);
    NAN_EXPORT(target, chattr);
    NAN_EXPORT(target, get);
    NAN_EXPORT(target, getStamp);
    NAN_EXPORT(target, delta);
    NAN_EXPORT(target, computeDeltaLocals);
    NAN_EXPORT(target, applyDelta);
    NAN_EXPORT(target, metaAdd);
    NAN_EXPORT(target, metaSet);
    NAN_EXPORT(target, metaRemove);
    NAN_EXPORT(target, metaGet);
    NAN_EXPORT(target, metaUnset);
    NAN_EXPORT(target, metaList);
    NAN_EXPORT(target, metaDump);
    NAN_EXPORT(target, stac);

    NAN_EXPORT(target, _shell_SHFileOperation);
    NAN_EXPORT(target, _shell_AltPress);
    NAN_EXPORT(target, _shell_AltRelease);


	DDBRegisterProcess();
}

NODE_MODULE(ddb, InitAll)
