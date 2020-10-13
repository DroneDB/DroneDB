/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include <sstream>
#include "ddb.h"
#include "ne_dbops.h"
#include "ne_helpers.h"

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
    ASSERT_NUM_PARAMS(2);

    BIND_STRING_PARAM(directory, 0);
    BIND_FUNCTION_PARAM(callback, 1);

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

      if (DDBAdd(ddbPath.c_str(), cPaths.data(), static_cast<int>(cPaths.size()), &output, recursive) != DDBERR_NONE){
          SetErrorMessage(DDBGetLastError());
      }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     Nan::JSON json;
     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         json.Parse(Nan::New<v8::String>(output).ToLocalChecked()).ToLocalChecked()
     };
     delete output; // TODO: is this a leak if the call fails? How do we de-allocate on failure?
     callback->Call(2, argv, async_resource);
   }

 private:
    char *output;

    std::string ddbPath;
    std::vector<std::string> paths;
    bool recursive;
};


NAN_METHOD(add) {
    ASSERT_NUM_PARAMS(4);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_ARRAY_PARAM(paths, 1);

    BIND_OBJECT_PARAM(obj, 2);
    BIND_OBJECT_VAR(obj, bool, recursive, false);

    BIND_FUNCTION_PARAM(callback, 3);

    Nan::AsyncQueueWorker(new AddWorker(callback, ddbPath, paths, recursive));
}


class RemoveWorker : public Nan::AsyncWorker {
 public:
  RemoveWorker(Nan::Callback *callback, const std::string &ddbPath, const std::vector<std::string> &paths)
    : AsyncWorker(callback, "nan:RemoveWorker"),
      ddbPath(ddbPath), paths(paths) {}
  ~RemoveWorker() {}

  void Execute () {
      std::vector<const char *> cPaths(paths.size());
      std::transform(paths.begin(), paths.end(), cPaths.begin(), [](const std::string& s) { return s.c_str(); });

      if (DDBRemove(ddbPath.c_str(), cPaths.data(), static_cast<int>(cPaths.size())) != DDBERR_NONE){
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
};


NAN_METHOD(remove) {
    ASSERT_NUM_PARAMS(4);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_ARRAY_PARAM(paths, 1);

    // TODO: this is not needed?
    //BIND_OBJECT_PARAM(obj, 2);

    BIND_FUNCTION_PARAM(callback, 3);

    Nan::AsyncQueueWorker(new RemoveWorker(callback, ddbPath, paths));
}

class ListWorker : public Nan::AsyncWorker {
 public:
  ListWorker(Nan::Callback *callback, const std::string &ddbPath, const std::vector<std::string> &paths, 
     bool recursive, int maxRecursionDepth)
    : AsyncWorker(callback, "nan:ListWorker"),
      ddbPath(ddbPath), paths(paths), recursive(recursive), maxRecursionDepth(maxRecursionDepth) {}
  ~ListWorker() {}

  void Execute () {
         
    std::vector<const char *> cPaths(paths.size());
    std::transform(paths.begin(), paths.end(), cPaths.begin(), [](const std::string& s) { return s.c_str(); });

    if (DDBList(ddbPath.c_str(), cPaths.data(), static_cast<int>(cPaths.size()), &output, "json", recursive, maxRecursionDepth) != DDBERR_NONE){
        SetErrorMessage(DDBGetLastError());
    }

  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     Nan::JSON json;
     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         json.Parse(Nan::New<v8::String>(output).ToLocalChecked()).ToLocalChecked()
     };

     delete output; // TODO: is this a leak if the call fails? How do we de-allocate on failure?
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::vector<std::string> paths;

    bool recursive;
    int maxRecursionDepth;   
    char *output;

};

NAN_METHOD(list) {
    ASSERT_NUM_PARAMS(4);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_ARRAY_PARAM(in, 1);
    BIND_OBJECT_PARAM(obj, 2);
    BIND_OBJECT_VAR(obj, bool, recursive, false);
    BIND_OBJECT_VAR(obj, int, maxRecursionDepth, 0);
    BIND_FUNCTION_PARAM(callback, 3);

    Nan::AsyncQueueWorker(new ListWorker(callback, ddbPath, in, recursive, maxRecursionDepth));
}