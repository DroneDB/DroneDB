/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <sstream>
#include "ne_shell.h"
#include "ne_helpers.h"
#include "exceptions.h"

#ifdef WIN32

#include <locale>
#include <codecvt>
#include <string>
#include <iostream>

#include <winuser.h>
#include <shellapi.h>

// Returns the last Win32 error, in string format. Returns an empty string if
// there is no error.
std::string GetErrorAsString(DWORD errorMessageID) {
    // Get the error message ID, if any.
    //DWORD errorMessageID = ::GetLastError();
    if (errorMessageID == 0) {
        return std::string();  // No error message has been recorded
    }

    LPSTR messageBuffer = nullptr;

    // Ask Win32 to give us the string version of that message ID.
    // The parameters we pass in, tell Win32 to create the buffer that holds the
    // message for us (because we don't yet know how long the message string will
    // be).
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&messageBuffer, 0, NULL);

    // Copy the error message into a std::string.
    std::string message(messageBuffer, size);

    // Free the Win32's string's buffer.
    LocalFree(messageBuffer);

    return message;
}

class SHFileOperationWorker : public Nan::AsyncWorker {
   public:
    SHFileOperationWorker(Nan::Callback *callback, const std::string &operation,
               const std::vector<std::string> &from, const std::string &to, int winId)
        : AsyncWorker(callback, "nan:ListWorker"),
          operation(operation),
          from(from),
          to(to),
          winId(winId), success(false) {}
    ~SHFileOperationWorker() {}

    void Execute() {
        HWND parentWindow =
            winId != 0 ? reinterpret_cast<HWND>(winId) : GetForegroundWindow();
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        
        size_t wFromLength = 0;
        for (auto &o : from) {
            wFromLength += o.size() + 1;
        }
        wFromLength++;
        
        wchar_t *wFrom = new wchar_t[wFromLength];

        size_t i = 0;
        for (auto &o : from) {
            std::replace(o.begin(), o.end(), '/', '\\');
            std::wstring co = converter.from_bytes(o);
            i += co.copy(wFrom + i, co.size(), 0);
            wFrom[i++] = NULL;
        }
        wFrom[i++] = NULL;

        std::replace(to.begin(), to.end(), '/', '\\');
        wchar_t *wTo = new wchar_t[to.size() + 2];
        std::wstring co = converter.from_bytes(to);

        i = 0;
        i += co.copy(wTo, to.size(), 0);
        wTo[i++] = NULL;
        wTo[i++] = NULL;


        SHFILEOPSTRUCTW op = {0};
        op.hwnd = parentWindow;
        if (operation == "copy")
            op.wFunc = FO_COPY;
        else if (operation == "move")
            op.wFunc = FO_MOVE;
        else if (operation == "delete")
            op.wFunc = FO_DELETE;
        else if (operation == "rename")
            op.wFunc = FO_RENAME;
        else {
            success = false;
            SetErrorMessage(
                std::string("Invalid operation " + operation).c_str());
            return;
        }

        op.pFrom = wFrom;
        op.pTo = wTo;
        op.fFlags = FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION;

        int ret = SHFileOperationW(&op);
        success = ret == 0;

        delete[] wFrom;
        delete[] wTo;

        if (!success) {
            SetErrorMessage(GetErrorAsString(ret).c_str());
        }
    }

    void HandleOKCallback() {
        Nan::HandleScope scope;

        Nan::JSON json;
        v8::Local<v8::Value> argv[] = {
            Nan::Null(), Nan::New<v8::Boolean>(success)
        };

        callback->Call(2, argv, async_resource);
    }

   private:
    std::string operation;
    std::vector<std::string> from;
    std::string to;

    int winId;

    bool success;
};

NAN_METHOD(_shell_SHFileOperation) {
    ASSERT_NUM_PARAMS(5);

    BIND_STRING_PARAM(operation, 0);
    BIND_STRING_ARRAY_PARAM(from, 1);
    BIND_STRING_PARAM(to, 2);

    BIND_OBJECT_PARAM(obj, 3);
    BIND_OBJECT_VAR(obj, int, winId, 0);

    BIND_FUNCTION_PARAM(callback, 4);

    Nan::AsyncQueueWorker(new SHFileOperationWorker(
        callback, operation, from, to, winId));
}


#else
NAN_METHOD(_shell_SHFileOperation) { throw std::Exception("Not implemented"); }
#endif


