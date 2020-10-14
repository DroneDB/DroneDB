/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include "registry.h"
#include "exceptions.h"
#include "ne_login.h"
#include "ne_helpers.h"


class LoginWorker : public Nan::AsyncWorker {
 public:
  LoginWorker(Nan::Callback *callback, const std::string &username,
              const std::string &password,
              const std::string &server)
    : AsyncWorker(callback, "nan:LoginWorker"),
      username(username), password(password), server(server){}
  ~LoginWorker() {}

  void Execute () {
    try{
        ddb::Registry reg(server);
        token = reg.login(username, password);

        if (token.length() == 0){
            SetErrorMessage("Unauthorized");
        }
    }catch(ddb::AppException &e){
        SetErrorMessage(e.what());
    }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     Nan::JSON json;
     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::New<v8::String>(token).ToLocalChecked()
     };

     callback->Call(2, argv, async_resource);
   }

 private:
    std::string token = "";

    std::string username;
    std::string password;
    std::string server;

};



NAN_METHOD(login) {
    ASSERT_NUM_PARAMS(4);

    BIND_STRING_PARAM(username, 0);
    BIND_STRING_PARAM(password, 1);
    BIND_STRING_PARAM(server, 2);
    BIND_FUNCTION_PARAM(callback, 3);

    Nan::AsyncQueueWorker(new LoginWorker(callback, username, password, server));
}
