/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include "ddb.h"
#include "ne_dbops.h"

class InitWorker : public Nan::AsyncWorker {
 public:
  InitWorker(Nan::Callback *callback, const std::string &directory)
    : AsyncWorker(callback, "nan:InitWorker"),
      directory(directory) {}
  ~InitWorker() {}

  void Execute () {
    char *outPath;

    if (DDBInit(directory.c_str(), &outPath) != DDBERR_NONE){
      SetErrorMessage(DDBGetLastError());
    }

    ddbPath = std::string(outPath);
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::New(ddbPath).ToLocalChecked()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string directory;
    std::string ddbPath;
};


NAN_METHOD(init) {
    if (info.Length() != 2){
        Nan::ThrowError("Invalid number of arguments");
        return;
    }
    if (!info[0]->IsString()){
        Nan::ThrowError("Argument 0 must be a string");
        return;
    }
    if (!info[1]->IsFunction()){
        Nan::ThrowError("Argument 1 must be a function");
        return;
    }

    std::string directory = *Nan::Utf8String(info[0].As<v8::String>());

    // Execute
    Nan::Callback *callback = new Nan::Callback(Nan::To<v8::Function>(info[1]).ToLocalChecked());
    Nan::AsyncQueueWorker(new InitWorker(callback, directory));
}


class AddWorker : public Nan::AsyncWorker {
 public:
  AddWorker(Nan::Callback *callback, const std::string &ddbPath, const std::vector<std::string> &paths, bool recursive)
    : AsyncWorker(callback, "nan:AddWorker"),
      ddbPath(ddbPath), paths(paths), recursive(recursive) {}
  ~AddWorker() {}

  void Execute () {
      std::vector<const char *> cPaths(paths.size());
      std::transform(paths.begin(), paths.end(), cPaths.begin(), [](const std::string& s) { return s.c_str(); });

      if (DDBAdd(ddbPath.c_str(), cPaths.data(), static_cast<int>(cPaths.size()), recursive) != DDBERR_NONE){
          SetErrorMessage(DDBGetLastError());
      }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::True()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::vector<std::string> paths;
    bool recursive;
};


NAN_METHOD(add) {
    if (info.Length() != 4){
        Nan::ThrowError("Invalid number of arguments");
        return;
    }
    if (!info[0]->IsString()){
        Nan::ThrowError("Argument 0 must be a string");
        return;
    }
    if (!info[1]->IsArray()){
        Nan::ThrowError("Argument 1 must be an array");
        return;
    }
    if (!info[2]->IsObject()){
        Nan::ThrowError("Argument 2 must be an object");
        return;
    }
    if (!info[3]->IsFunction()){
        Nan::ThrowError("Argument 3 must be a function");
        return;
    }

    std::string ddbPath = *Nan::Utf8String(info[0].As<v8::String>());

    // v8 array --> c++ std vector
    auto p = info[1].As<v8::Array>();
    auto ctx = info.GetIsolate()->GetCurrentContext();
    std::vector<std::string> paths;
    for (unsigned int i = 0; i < p->Length(); i++){
        Nan::Utf8String str(p->Get(ctx, i).ToLocalChecked());
        paths.push_back(std::string(*str));
    }

    // Parse options
    v8::Local<v8::String> k = Nan::New<v8::String>("recursive").ToLocalChecked();
    v8::Local<v8::Object> obj = info[2].As<v8::Object>();

    bool recursive = false;
    if (Nan::HasRealNamedProperty(obj, k).FromJust()){
        recursive = Nan::To<bool>(Nan::GetRealNamedProperty(obj, k).ToLocalChecked()).FromJust();
    }

    // Execute
    Nan::Callback *callback = new Nan::Callback(Nan::To<v8::Function>(info[3]).ToLocalChecked());
    Nan::AsyncQueueWorker(new AddWorker(callback, ddbPath, paths, recursive));
}


class RemoveWorker : public Nan::AsyncWorker {
 public:
  RemoveWorker(Nan::Callback *callback, const std::string &ddbPath, const std::vector<std::string> &paths, bool recursive)
    : AsyncWorker(callback, "nan:RemoveWorker"),
      ddbPath(ddbPath), paths(paths), recursive(recursive) {}
  ~RemoveWorker() {}

  void Execute () {
      std::vector<const char *> cPaths(paths.size());
      std::transform(paths.begin(), paths.end(), cPaths.begin(), [](const std::string& s) { return s.c_str(); });

      if (DDBRemove(ddbPath.c_str(), cPaths.data(), static_cast<int>(cPaths.size()), recursive) != DDBERR_NONE){
          SetErrorMessage(DDBGetLastError());
      }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::True()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::vector<std::string> paths;
    bool recursive;
};


NAN_METHOD(remove) {
    if (info.Length() != 4){
        Nan::ThrowError("Invalid number of arguments");
        return;
    }
    if (!info[0]->IsString()){
        Nan::ThrowError("Argument 0 must be a string");
        return;
    }
    if (!info[1]->IsArray()){
        Nan::ThrowError("Argument 1 must be an array");
        return;
    }
    if (!info[2]->IsObject()){
        Nan::ThrowError("Argument 2 must be an object");
        return;
    }
    if (!info[3]->IsFunction()){
        Nan::ThrowError("Argument 3 must be a function");
        return;
    }

    std::string ddbPath = *Nan::Utf8String(info[0].As<v8::String>());

    // v8 array --> c++ std vector
    auto p = info[1].As<v8::Array>();
    auto ctx = info.GetIsolate()->GetCurrentContext();
    std::vector<std::string> paths;
    for (unsigned int i = 0; i < p->Length(); i++){
        Nan::Utf8String str(p->Get(ctx, i).ToLocalChecked());
        paths.push_back(std::string(*str));
    }

    // Parse options
    v8::Local<v8::String> k = Nan::New<v8::String>("recursive").ToLocalChecked();
    v8::Local<v8::Object> obj = info[2].As<v8::Object>();

    bool recursive = false;
    if (Nan::HasRealNamedProperty(obj, k).FromJust()){
        recursive = Nan::To<bool>(Nan::GetRealNamedProperty(obj, k).ToLocalChecked()).FromJust();
    }

    // Execute
    Nan::Callback *callback = new Nan::Callback(Nan::To<v8::Function>(info[3]).ToLocalChecked());
    Nan::AsyncQueueWorker(new RemoveWorker(callback, ddbPath, paths, recursive));
}
