#include "functions.h"
#include "../libs/ddb.h"

using v8::FunctionTemplate;

// NativeExtension.cc represents the top level of the module.
// C++ constructs that are exposed to javascript are exported here

NAN_MODULE_INIT(InitAll) {
  Nan::Set(target, Nan::New("getVersion").ToLocalChecked(),
    Nan::GetFunction(Nan::New<FunctionTemplate>(getVersion)).ToLocalChecked());
  Nan::Set(target, Nan::New("typeToHuman").ToLocalChecked(),
    Nan::GetFunction(Nan::New<FunctionTemplate>(typeToHuman)).ToLocalChecked());
  Nan::Set(target, Nan::New("parseFiles").ToLocalChecked(),
    Nan::GetFunction(Nan::New<FunctionTemplate>(parseFiles)).ToLocalChecked());
  Nan::Set(target, Nan::New("_thumbs_getFromUserCache").ToLocalChecked(),
    Nan::GetFunction(Nan::New<FunctionTemplate>(_thumbs_getFromUserCache)).ToLocalChecked());

  // Nan::Set(target, Nan::New("callback").ToLocalChecked(),
  //   Nan::GetFunction(Nan::New<FunctionTemplate>(callback)).ToLocalChecked());
  // Nan::Set(target, Nan::New("callbackWithParameter").ToLocalChecked(),
  //   Nan::GetFunction(Nan::New<FunctionTemplate>(callbackWithParameter)).ToLocalChecked());

  // Passing target down to the next NAN_MODULE_INIT
  // MyObject::Init(target);
  ddb::initialize();
}

NODE_MODULE(NativeExtension, InitAll)
