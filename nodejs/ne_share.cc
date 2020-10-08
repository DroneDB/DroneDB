/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include "shareservice.h"
#include "exceptions.h"
#include "ne_share.h"

class ShareWorker : public Nan::AsyncWorker {
 public:
  ShareWorker(Nan::Callback *callback, const std::vector<std::string> &input, const std::string &tag, const std::string &password,
              bool recursive, )
    : AsyncWorker(callback, "nan:ShareWorker"),
      input(input), tag(tag), password(password),
      recursive(recursive) {}
  ~ShareWorker() {}

  void Execute () {
    ddb::ShareService ss;
    try{
        ss.share(input, tag, password, recursive, );
    }catch(const ddb::AuthException &){
        SetErrorMessage("Unauthorized");
    }catch(const ddb::AppException &e){
        SetErrorMessage(e.what());
    }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::New(url).ToLocalChecked()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    std::vector<std::string> input;
    std::string tag;
    std::string password;
    bool recursive;

    std::string url;
};


NAN_METHOD(share) {
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
    Nan::AsyncQueueWorker(new ShareWorker(callback, directory));
}
