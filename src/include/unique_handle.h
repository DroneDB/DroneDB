/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef UNIQUE_HANDLE_H
#define UNIQUE_HANDLE_H

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ddb {

// Sole owner of an operating-system handle, closed when the owner is destroyed or reset.
// Move-only, like std::unique_ptr; a moved-from owner holds the invalid value. Traits
// provide: `using Handle`, `static Handle invalid()`, `static bool isValid(Handle)` and
// `static void close(Handle)`.
template <typename Traits>
class UniqueOsHandle {
public:
    using Handle = typename Traits::Handle;

    UniqueOsHandle() noexcept = default;
    explicit UniqueOsHandle(Handle handle) noexcept : handle_(handle) {}
    ~UniqueOsHandle() { reset(); }

    UniqueOsHandle(const UniqueOsHandle&) = delete;
    UniqueOsHandle& operator=(const UniqueOsHandle&) = delete;
    UniqueOsHandle(UniqueOsHandle&& other) noexcept : handle_(other.release()) {}
    UniqueOsHandle& operator=(UniqueOsHandle&& other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    Handle get() const noexcept { return handle_; }
    bool valid() const noexcept { return Traits::isValid(handle_); }
    explicit operator bool() const noexcept { return valid(); }

    // Gives up ownership without closing
    Handle release() noexcept {
        const Handle handle = handle_;
        handle_ = Traits::invalid();
        return handle;
    }

    // Closes the owned handle (if valid) and takes ownership of `handle`
    void reset(Handle handle = Traits::invalid()) noexcept {
        const Handle old = handle_;
        handle_ = handle;
        if (Traits::isValid(old))
            Traits::close(old);
    }

    // Closes the owned handle and returns the address of the empty slot, for APIs that
    // return a new handle through an out-parameter (CreatePipe)
    Handle* put() noexcept {
        reset();
        return &handle_;
    }

private:
    Handle handle_ = Traits::invalid();
};

#ifdef _WIN32
struct WinHandleTraits {
    using Handle = HANDLE;
    static Handle invalid() noexcept { return nullptr; }
    // CreateFile reports a failure with INVALID_HANDLE_VALUE, most other APIs with nullptr
    static bool isValid(Handle h) noexcept { return h != nullptr && h != INVALID_HANDLE_VALUE; }
    static void close(Handle h) noexcept { CloseHandle(h); }
};

// A Win32 kernel object handle (pipe, file, process, thread, job)
using UniqueHandle = UniqueOsHandle<WinHandleTraits>;
#else
struct PosixFdTraits {
    using Handle = int;
    static Handle invalid() noexcept { return -1; }
    static bool isValid(Handle fd) noexcept { return fd >= 0; }
    static void close(Handle fd) noexcept { ::close(fd); }
};

// A POSIX file descriptor
using UniqueFd = UniqueOsHandle<PosixFdTraits>;
#endif

}  // namespace ddb

#endif  // UNIQUE_HANDLE_H
