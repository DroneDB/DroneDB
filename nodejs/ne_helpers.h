/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef NE_HELPERS_H
#define NE_HELPERS_H

#include <nan.h>

// v8 array --> c++ std vector
inline std::vector<std::string> v8ArrayToStrVec(const v8::Local<v8::Array> &array){
    auto ctx = array->GetIsolate()->GetCurrentContext();
    std::vector<std::string> in;
    for (unsigned int i = 0; i < array->Length(); i++){
        Nan::Utf8String str(array->Get(ctx, i).ToLocalChecked());
        in.push_back(std::string(*str));
    }
    return in;
}

#define ASSERT_NUM_PARAMS(__num){ \
    if (info.Length() != __num){ \
        Nan::ThrowError("Invalid number of arguments"); \
        return; \
    }\
}

#define BIND_STRING_ARRAY_PARAM(__name, __num) \
    if (!info[__num]->IsArray()){ \
        Nan::ThrowError("Argument " #__num " must be an array"); \
        return; \
    }\
    auto __name = v8ArrayToStrVec(info[__num].As<v8::Array>());

#define BIND_OBJECT_PARAM(__name, __num) \
    if (!info[__num]->IsObject()){ \
        Nan::ThrowError("Argument " #__num " must be an object"); \
        return; \
    } \
    v8::Local<v8::Object> __name = info[__num].As<v8::Object>();

#define BIND_OBJECT_VAR(__obj, __type, __name, __defaultValue) \
    v8::Local<v8::String> __name ## key = Nan::New<v8::String>(#__name).ToLocalChecked(); \
    __type __name = __defaultValue; \
    if (Nan::HasRealNamedProperty(__obj, __name ## key).FromJust()){ \
        __name = Nan::To<__type>(Nan::GetRealNamedProperty(__obj, __name ## key).ToLocalChecked()).FromJust(); \
    }

#define BIND_OBJECT_STRING(__obj, __name, __defaultValue) \
    v8::Local<v8::String> __name ## key = Nan::New<v8::String>(#__name).ToLocalChecked(); \
    std::string __name = __defaultValue; \
    if (Nan::HasRealNamedProperty(__obj, __name ## key).FromJust()){ \
        Nan::Utf8String str(Nan::GetRealNamedProperty(__obj, __name ## key).ToLocalChecked().As<v8::String>()); \
        __name = std::string(*str); \
    }

#define BIND_FUNCTION_PARAM(__name, __num) \
    if (!info[__num]->IsFunction()){ \
        Nan::ThrowError("Argument " #__num " must be a function"); \
        return; \
    } \
    Nan::Callback *__name = new Nan::Callback(Nan::To<v8::Function>(info[__num]).ToLocalChecked());

#define BIND_STRING_PARAM(__name, __num) \
    if (!info[__num]->IsString()){ \
        Nan::ThrowError("Argument " #__num " must be a string"); \
        return; \
    } \
    std::string __name = *Nan::Utf8String(info[__num].As<v8::String>());

#define BIND_UNSIGNED_INT_PARAM(__name, __num) \
    if (!info[__num]->IsNumber()){ \
        Nan::ThrowError("Argument " #__num " must be a number"); \
        return; \
    } \
    uint32_t __name = Nan::To<uint32_t>(info[__num].As<v8::Uint32>()).FromJust();

#define BIND_INT_PARAM(__name, __num) \
    if (!info[__num]->IsNumber()){ \
        Nan::ThrowError("Argument " #__num " must be a number"); \
        return; \
    } \
    int __name = Nan::To<int>(info[__num].As<v8::Int32>()).FromJust();


#endif
