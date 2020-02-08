#include <sstream>

#include "functions.h"
#include "../libs/ddb.h"
#include "../libs/info.h"

NAN_METHOD(getVersion) {
    info.GetReturnValue().Set(Nan::New(ddb::getVersion()).ToLocalChecked());
}

class ParseFilesWorker : public Nan::AsyncWorker {
 public:
  ParseFilesWorker(Nan::Callback *callback, const std::vector<std::string> &input, bool computeHash,  bool recursive)
    : AsyncWorker(callback, "nan:ParseFilesWorker"),
      input(input), computeHash(computeHash), recursive(recursive){}
  ~ParseFilesWorker() {}

  void Execute () {
    try{
        ddb::parseFiles(input, "json", s, computeHash, recursive);
    }catch(ddb::AppException &e){
        SetErrorMessage(e.what());
    }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     Nan::JSON json;
     Nan::MaybeLocal<v8::Value> result = json.Parse(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), s.str().c_str()).ToLocalChecked());

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         result.ToLocalChecked()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    std::vector<std::string> input;
    bool computeHash;
    bool recursive;
    std::ostringstream s;
};


NAN_METHOD(parseFiles) {
    if (info.Length() != 4){
        Nan::ThrowError("Invalid number of arguments");
        return;
	}
    if (!info[0]->IsArray()){
        Nan::ThrowError("Argument 0 must be an array");
        return;
    }
    if (!info[3]->IsFunction()){
        Nan::ThrowError("Argument 3 must be a function");
        return;
    }

    // v8 array --> c++ std vector
    auto input = info[0].As<v8::Array>();
    auto ctx = info.GetIsolate()->GetCurrentContext();
    std::vector<std::string> in;
    for (unsigned int i = 0; i < input->Length(); i++){
        Nan::Utf8String str(input->Get(ctx, i).ToLocalChecked());
        in.push_back(std::string(*str));
    }

    Nan::Callback *callback = new Nan::Callback(Nan::To<v8::Function>(info[3]).ToLocalChecked());
    Nan::AsyncQueueWorker(new ParseFilesWorker(callback, in, Nan::To<bool>(info[1]).FromJust(), Nan::To<bool>(info[2]).FromJust()));
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
