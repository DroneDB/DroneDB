#include "functions.h"
#include "../libs/ddb.h"

NAN_METHOD(getVersion) {
    info.GetReturnValue().Set(Nan::New(ddb::getVersion()).ToLocalChecked());
}

// NAN_METHOD(aBoolean) {
//     info.GetReturnValue().Set(false);
// }

// NAN_METHOD(aNumber) {
//     info.GetReturnValue().Set(1.75);
// }

// NAN_METHOD(anObject) {
//     v8::Local<v8::Object> obj = Nan::New<v8::Object>();
//     Nan::Set(obj, Nan::New("key").ToLocalChecked(), Nan::New("value").ToLocalChecked());
//     info.GetReturnValue().Set(obj);
// }

// NAN_METHOD(anArray) {
//     v8::Local<v8::Array> arr = Nan::New<v8::Array>(3);
//     Nan::Set(arr, 0, Nan::New(1));
//     Nan::Set(arr, 1, Nan::New(2));
//     Nan::Set(arr, 2, Nan::New(3));
//     info.GetReturnValue().Set(arr);
// }

// NAN_METHOD(callback) {
//     v8::Local<v8::Function> callbackHandle = info[0].As<v8::Function>();
//     Nan::AsyncResource* resource = new Nan::AsyncResource(Nan::New<v8::String>("MyObject:CallCallback").ToLocalChecked());
//     resource->runInAsyncScope(Nan::GetCurrentContext()->Global(), callbackHandle, 0, 0);
// }

// NAN_METHOD(callbackWithParameter) {
//     v8::Local<v8::Function> callbackHandle = info[0].As<v8::Function>();
//     Nan::AsyncResource* resource = new Nan::AsyncResource(Nan::New<v8::String>("MyObject:CallCallbackWithParam").ToLocalChecked());
//     int argc = 1;
//     v8::Local<v8::Value> argv[] = {
//         Nan::New("parameter test").ToLocalChecked()
//     };
//     resource->runInAsyncScope(Nan::GetCurrentContext()->Global(), callbackHandle, argc, argv);
// }

// // Wrapper Impl

// Nan::Persistent<v8::Function> MyObject::constructor;

// NAN_MODULE_INIT(MyObject::Init) {
//   v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
//   tpl->SetClassName(Nan::New("MyObject").ToLocalChecked());
//   tpl->InstanceTemplate()->SetInternalFieldCount(1);

//   Nan::SetPrototypeMethod(tpl, "plusOne", PlusOne);

//   constructor.Reset(Nan::GetFunction(tpl).ToLocalChecked());
//   Nan::Set(target, Nan::New("MyObject").ToLocalChecked(), Nan::GetFunction(tpl).ToLocalChecked());
// }

// MyObject::MyObject(double value) : value_(value) {
// }

// MyObject::~MyObject() {
// }

// NAN_METHOD(MyObject::New) {
//   if (info.IsConstructCall()) {
//     double value = info[0]->IsUndefined() ? 0 : Nan::To<double>(info[0]).FromJust();
//     MyObject *obj = new MyObject(value);
//     obj->Wrap(info.This());
//     info.GetReturnValue().Set(info.This());
//   } else {
//     const int argc = 1;
//     v8::Local<v8::Value> argv[argc] = {info[0]};
//     v8::Local<v8::Function> cons = Nan::New(constructor);
//     info.GetReturnValue().Set(Nan::NewInstance(cons, argc, argv).ToLocalChecked());
//   }
// }

// NAN_METHOD(MyObject::PlusOne) {
//   MyObject* obj = Nan::ObjectWrap::Unwrap<MyObject>(info.This());
//   obj->value_ += 1;
//   info.GetReturnValue().Set(obj->value_);
// }
