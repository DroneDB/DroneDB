/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NE_DBOPS_H
#define NE_DBOPS_H

#include <nan.h>

NAN_METHOD(init);
NAN_METHOD(add);
NAN_METHOD(remove);
NAN_METHOD(move);
NAN_METHOD(list);
NAN_METHOD(build);
NAN_METHOD(search);
NAN_METHOD(chattr);
NAN_METHOD(get);
NAN_METHOD(getStamp);
NAN_METHOD(delta);
NAN_METHOD(computeDeltaLocals);
NAN_METHOD(applyDelta);
NAN_METHOD(stac);


#endif
