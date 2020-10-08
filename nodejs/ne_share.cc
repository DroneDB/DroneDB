/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include "shareservice.h"
#include "exceptions.h"
#include "ne_share.h"
#include "ne_helpers.h"

class ShareWorker : public Nan::AsyncWorker {
 public:
  ShareWorker(Nan::Callback *callback, const std::vector<std::string> &input, const std::string &tag, const std::string &password,
              bool recursive)
    : AsyncWorker(callback, "nan:ShareWorker"),
      input(input), tag(tag), password(password),
      recursive(recursive) {}
  ~ShareWorker() {}

  void Execute () {
    ddb::ShareService ss;
    try{
        ddb::ShareCallback showProgress = [](const std::string &file, float progress){
            return true;
        };

        ss.share(input, tag, password, recursive, "", showProgress);
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
//    ASSERT_NUM_PARAMS(2);

//    Nan::AsyncQueueWorker(new ShareWorker(callback, params....));
}
