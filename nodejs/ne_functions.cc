/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <sstream>
#include "ne_functions.h"
#include "ne_helpers.h"
#include "constants.h"
#include "ddb.h"
#include "dbops.h"
#include "info.h"
#include "thumbs.h"
#include "entry.h"

NAN_METHOD(getVersion) {
    info.GetReturnValue().Set(Nan::New(DDBGetVersion()).ToLocalChecked());
}

NAN_METHOD(getDefaultRegistry){
    info.GetReturnValue().Set(Nan::New(DEFAULT_REGISTRY).ToLocalChecked());
}

class InfoWorker : public Nan::AsyncWorker {
 public:
  InfoWorker(Nan::Callback *callback, const std::vector<std::string> &input,
             bool recursive, int maxRecursionDepth, bool withHash, bool stopOnError)
    : AsyncWorker(callback, "nan:InfoWorker"),
      input(input), recursive(recursive), maxRecursionDepth(maxRecursionDepth),
      withHash(withHash), stopOnError(stopOnError){}
  ~InfoWorker() {}

  void Execute () {
    try{
        ddb::info(input, s, "json", recursive, maxRecursionDepth,
                  "auto", withHash, stopOnError);
    }catch(ddb::AppException &e){
        SetErrorMessage(e.what());
    }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     Nan::JSON json;
     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         json.Parse(Nan::New<v8::String>(s.str()).ToLocalChecked()).ToLocalChecked()
     };

     callback->Call(2, argv, async_resource);
   }

 private:
    std::vector<std::string> input;
    std::ostringstream s;

    bool recursive;
    int maxRecursionDepth;
    bool withHash;
    bool stopOnError;
};


NAN_METHOD(info) {
    ASSERT_NUM_PARAMS(3);

    BIND_STRING_ARRAY_PARAM(in, 0);

    BIND_OBJECT_PARAM(obj, 1);
    BIND_OBJECT_VAR(obj, bool, withHash, false);
    BIND_OBJECT_VAR(obj, bool, stopOnError, true);
    BIND_OBJECT_VAR(obj, bool, recursive, false);
    BIND_OBJECT_VAR(obj, int, maxRecursionDepth, 0);

    BIND_FUNCTION_PARAM(callback, 2);

    // Execute
    Nan::AsyncQueueWorker(new InfoWorker(callback, in, recursive, maxRecursionDepth, withHash, stopOnError));
}

//(const fs::path &imagePath, time_t modifiedTime, int thumbSize, bool forceRecreate)
class GetThumbFromUserCacheWorker : public Nan::AsyncWorker {
 public:
  GetThumbFromUserCacheWorker(Nan::Callback *callback, const fs::path &imagePath, int thumbSize, bool forceRecreate)
    : AsyncWorker(callback, "nan:GetThumbFromUserCacheWorker"),
      imagePath(imagePath), thumbSize(thumbSize), forceRecreate(forceRecreate) {}
  ~GetThumbFromUserCacheWorker() {}

  void Execute () {
    try{
        thumbPath = ddb::getThumbFromUserCache(imagePath, thumbSize, forceRecreate);
    }catch(ddb::AppException &e){
        SetErrorMessage(e.what());
    }
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::New(thumbPath.string()).ToLocalChecked()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    fs::path imagePath;
    int thumbSize;
    bool forceRecreate;

    fs::path thumbPath;
};


NAN_METHOD(_thumbs_getFromUserCache) {
    ASSERT_NUM_PARAMS(3);

    BIND_STRING_PARAM(imagePath, 0);

    BIND_OBJECT_PARAM(obj, 1);
    BIND_OBJECT_VAR(obj, int, thumbSize, 512);
    BIND_OBJECT_VAR(obj, bool, forceRecreate, false);

    BIND_FUNCTION_PARAM(callback, 2);

    Nan::AsyncQueueWorker(new GetThumbFromUserCacheWorker(callback, static_cast<fs::path>(imagePath), thumbSize, forceRecreate));
}

class GetTileFromUserCacheWorker : public Nan::AsyncWorker {
 public:
  GetTileFromUserCacheWorker(Nan::Callback *callback, const std::string &geotiffPath, int tz, int tx, int ty, int tileSize, bool tms, bool forceRecreate)
    : AsyncWorker(callback, "nan:GetTileFromUserCacheWorker"),
      geotiffPath(geotiffPath), tz(tz), tx(tx), ty(ty), tileSize(tileSize), tms(tms), forceRecreate(forceRecreate) {}
  ~GetTileFromUserCacheWorker() {}

  void Execute () {
    char *outputTilePath;

    if (DDBTile(geotiffPath.c_str(), tz, tx, ty, &outputTilePath, tileSize, tms, forceRecreate) != DDBERR_NONE){
      SetErrorMessage(DDBGetLastError());
    }

    tilePath = std::string(outputTilePath);
  }

  void HandleOKCallback () {
     Nan::HandleScope scope;

     v8::Local<v8::Value> argv[] = {
         Nan::Null(),
         Nan::New(tilePath).ToLocalChecked()
     };
     callback->Call(2, argv, async_resource);
   }

 private:
    std::string geotiffPath;
    int tz, tx, ty;
    int tileSize;
    bool tms;
    bool forceRecreate;

    std::string tilePath;
};


NAN_METHOD(_tile_getFromUserCache) {
    ASSERT_NUM_PARAMS(6);

    BIND_STRING_PARAM(geotiffPath, 0);
    BIND_INT_PARAM(tz, 1);
    BIND_INT_PARAM(tx, 2);
    BIND_INT_PARAM(ty, 3);

    BIND_OBJECT_PARAM(obj, 4);
    BIND_OBJECT_VAR(obj, int, tileSize, 256);
    BIND_OBJECT_VAR(obj, bool, tms, false);
    BIND_OBJECT_VAR(obj, bool, forceRecreate, false);

    BIND_FUNCTION_PARAM(callback, 5);

    Nan::AsyncQueueWorker(new GetTileFromUserCacheWorker(callback, geotiffPath, tz, tx, ty, tileSize, tms, forceRecreate));
}

