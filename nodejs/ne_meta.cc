/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iostream>
#include <sstream>
#include "ddb.h"
#include "ne_meta.h"
#include "ne_helpers.h"

class MetaAddWorker : public Nan::AsyncWorker {
 public:
  MetaAddWorker(Nan::Callback *callback,  const std::string &ddbPath,  const std::string &path, 
    const std::string &key, const std::string &data)
    : AsyncWorker(callback, "nan:MetaAddWorker"),
      ddbPath(ddbPath), path(path), key(key), data(data) {}
  ~MetaAddWorker() {}

  void Execute () {
    if (DDBMetaAdd(ddbPath.c_str(), path.c_str(), key.c_str(), data.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string path;
    std::string key;
    std::string data;

    char *output;
};


NAN_METHOD(metaAdd) {
    ASSERT_NUM_PARAMS(5);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(path, 1);
    BIND_STRING_PARAM(key, 2);
    BIND_STRING_PARAM(data, 3);

    BIND_FUNCTION_PARAM(callback, 4);

    Nan::AsyncQueueWorker(new MetaAddWorker(callback, ddbPath, path, key, data));
}


class MetaSetWorker : public Nan::AsyncWorker {
 public:
  MetaSetWorker(Nan::Callback *callback,  const std::string &ddbPath,  const std::string &path, 
    const std::string &key, const std::string &data)
    : AsyncWorker(callback, "nan:MetaSetWorker"),
      ddbPath(ddbPath), path(path), key(key), data(data) {}
  ~MetaSetWorker() {}

  void Execute () {
    if (DDBMetaSet(ddbPath.c_str(), path.c_str(), key.c_str(), data.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string path;
    std::string key;
    std::string data;

    char *output;
};


NAN_METHOD(metaSet) {
    ASSERT_NUM_PARAMS(5);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(path, 1);
    BIND_STRING_PARAM(key, 2);
    BIND_STRING_PARAM(data, 3);

    BIND_FUNCTION_PARAM(callback, 4);

    Nan::AsyncQueueWorker(new MetaSetWorker(callback, ddbPath, path, key, data));
}

class MetaRemoveWorker : public Nan::AsyncWorker {
 public:
  MetaRemoveWorker(Nan::Callback *callback,  const std::string &ddbPath, const std::string &id)
    : AsyncWorker(callback, "nan:MetaRemoveWorker"),
      ddbPath(ddbPath), id(id) {}
  ~MetaRemoveWorker() {}

  void Execute () {
    if (DDBMetaRemove(ddbPath.c_str(), id.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string id;

    char *output;
};


NAN_METHOD(metaRemove) {
    ASSERT_NUM_PARAMS(3);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(id, 1);

    BIND_FUNCTION_PARAM(callback, 2);

    Nan::AsyncQueueWorker(new MetaRemoveWorker(callback, ddbPath, id));
}

class MetaGetWorker : public Nan::AsyncWorker {
 public:
  MetaGetWorker(Nan::Callback *callback,  const std::string &ddbPath, const std::string &path, 
    const std::string &key)
    : AsyncWorker(callback, "nan:MetaGetWorker"),
      ddbPath(ddbPath), path(path), key(key) {}
  ~MetaGetWorker() {}

  void Execute () {
    if (DDBMetaGet(ddbPath.c_str(), path.c_str(), key.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string path;
    std::string key;

    char *output;
};


NAN_METHOD(metaGet) {
    ASSERT_NUM_PARAMS(4);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(path, 1);
    BIND_STRING_PARAM(key, 2);

    BIND_FUNCTION_PARAM(callback, 3);

    Nan::AsyncQueueWorker(new MetaGetWorker(callback, ddbPath, path, key));
}

class MetaUnsetWorker : public Nan::AsyncWorker {
 public:
  MetaUnsetWorker(Nan::Callback *callback,  const std::string &ddbPath, const std::string &path, 
    const std::string &key)
    : AsyncWorker(callback, "nan:MetaUnsetWorker"),
      ddbPath(ddbPath), path(path), key(key) {}
  ~MetaUnsetWorker() {}

  void Execute () {
    if (DDBMetaUnset(ddbPath.c_str(), path.c_str(), key.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string path;
    std::string key;

    char *output;
};


NAN_METHOD(metaUnset) {
    ASSERT_NUM_PARAMS(4);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(path, 1);
    BIND_STRING_PARAM(key, 2);

    BIND_FUNCTION_PARAM(callback, 3);

    Nan::AsyncQueueWorker(new MetaUnsetWorker(callback, ddbPath, path, key));
}

class MetaListWorker : public Nan::AsyncWorker {
 public:
  MetaListWorker(Nan::Callback *callback,  const std::string &ddbPath, const std::string &path)
    : AsyncWorker(callback, "nan:MetaListWorker"),
      ddbPath(ddbPath), path(path) {}
  ~MetaListWorker() {}

  void Execute () {
    if (DDBMetaList(ddbPath.c_str(), path.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string path;

    char *output;
};


NAN_METHOD(metaList) {
    ASSERT_NUM_PARAMS(3);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(path, 1);

    BIND_FUNCTION_PARAM(callback, 2);

    Nan::AsyncQueueWorker(new MetaListWorker(callback, ddbPath, path));
}

class MetaDumpWorker : public Nan::AsyncWorker {
 public:
  MetaDumpWorker(Nan::Callback *callback,  const std::string &ddbPath, const std::string &ids)
    : AsyncWorker(callback, "nan:MetaDumpWorker"),
      ddbPath(ddbPath), ids(ids) {}
  ~MetaDumpWorker() {}

  void Execute () {
    if (DDBMetaDump(ddbPath.c_str(), ids.c_str(), &output) != DDBERR_NONE){
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

     delete output;
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string ddbPath;
    std::string ids;

    char *output;
};


NAN_METHOD(metaDump) {
    ASSERT_NUM_PARAMS(3);

    BIND_STRING_PARAM(ddbPath, 0);
    BIND_STRING_PARAM(ids, 1);

    BIND_FUNCTION_PARAM(callback, 2);

    Nan::AsyncQueueWorker(new MetaDumpWorker(callback, ddbPath, ids));
}