/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "ne_helpers.h"

// v8 array --> c++ std vector
static inline std::vector<std::string> v8ArrayToStrVec(const v8::Local<v8::Array> &array){
    auto ctx = array->GetIsolate()->GetCurrentContext();
    std::vector<std::string> in;
    for (unsigned int i = 0; i < array->Length(); i++){
        Nan::Utf8String str(array->Get(ctx, i).ToLocalChecked());
        in.push_back(std::string(*str));
    }
    return in;
}


