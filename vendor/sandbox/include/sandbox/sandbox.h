/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Confinement of the helper programs libddb starts: they parse files a user supplied, so a
// parser that is exploited must find itself in a process that can neither reach the network,
// nor read other people's files, nor use more than a bounded share of the machine.
//
// The library depends on nothing of libddb and its header is plain C++17, so every helper
// program links it. apply() is meant to be called once, early, in a process that has just been
// started or forked for one job and has no other thread yet: the layers restrict the calling
// process (and the threads it creates later), and none of them can be lifted again. It does not
// throw and does not allocate more than a few strings, so it is safe in a freshly forked child.

#ifndef SANDBOX_SANDBOX_H
#define SANDBOX_SANDBOX_H

#include <cstdint>
#include <string>
#include <vector>

namespace sandbox
{
    // What to do about a layer the host cannot provide
    enum class Mode
    {
        Strict,     // refuse to start: apply() fails and names the layer
        BestEffort, // apply the layers that exist and report the missing ones (the default)
        Off         // apply no layer
    };

    // The mode a configuration value names: "strict", "best-effort" or "off" (any case, spaces
    // around ignored). No value or an empty one is BestEffort; so is an unknown one, and then
    // *warning (when not null) says what was wrong. *warning is cleared otherwise.
    Mode parseMode(const char *value, std::string *warning = nullptr);
    const char *modeName(Mode mode);

    // What the process may do once the layers are applied. A limit of 0 means none.
    struct Policy
    {
        // Folders (or single files) the process may read. A path that does not exist is
        // skipped: the list can name optional locations.
        std::vector<std::string> readPaths;
        // Folders (or single files) the process may read and write, create and delete in. They
        // must exist; a missing one is an error in every mode but Off.
        std::vector<std::string> writePaths;

        uint64_t maxMemoryBytes = 0; // address space of the process
        uint64_t maxCpuSeconds = 0;  // CPU time of the process (it ends with SIGXCPU on Linux)
        uint64_t maxFileBytes = 0;   // size of a file the process writes
        uint64_t maxOpenFiles = 0;   // file descriptors

        // Layers to treat as unavailable, whatever the host offers: lets a test see how a host
        // without a layer behaves. Names are the ones of LayerState::name.
        std::vector<std::string> unavailableLayers;
    };

    // One confinement mechanism
    struct LayerState
    {
        std::string name;   // "rlimits", "no-new-privs", "seccomp", "landlock", "oom-score",
                            // "job-object", "integrity-level"
        bool active = false; // applied (by apply) or applicable (by probe)
        std::string detail;  // why it is missing, or what it covers
    };

    struct Report
    {
        Mode mode = Mode::BestEffort;
        std::vector<LayerState> layers;
        // False when the process must not go on: Strict and a layer is missing, or a path of the
        // policy cannot be confined. The process has to exit then; error says why.
        bool ok = true;
        std::string error;

        // The layers that are not active
        std::vector<std::string> missingLayers() const;
        // One line naming every missing layer and why, for the one warning per process that
        // BestEffort calls for; empty when nothing is missing
        std::string warning() const;
        // One line per layer, "<name>: active|missing[ (<detail>)]", for a report command
        std::string describe() const;
    };

    // Applies the layers of this platform to the calling process. Strict checks that every
    // layer is available before it applies any, and fails without restricting anything when one
    // is not. Off applies nothing and reports no layer.
    Report apply(const Policy &policy, Mode mode);

    // The layers this host offers, without restricting the process
    Report probe();

    // What the sanitizer runtime this build is compiled with cannot live with, and the layers
    // therefore leave out, in such a build only: the detail of the layer says so, in the report
    // of apply() and of probe(). A build without a sanitizer leaves out nothing, and its layers
    // are the same as if this did not exist.
    struct SanitizerAllowances
    {
        // AddressSanitizer, ThreadSanitizer and MemorySanitizer reserve terabytes of address
        // space for their shadow memory when the process starts, so a limit of the address space
        // (RLIMIT_AS) would make every later mapping fail: maxMemoryBytes is not applied. The
        // Job Object of Windows limits the memory a process commits, which the shadow memory is
        // not until it is used, and keeps its limit.
        bool unlimitedAddressSpace = false;
        // LeakSanitizer checks a process for leaks when it exits, from a process it clones that
        // shares its memory, lists the threads in /proc/self/task, stops them with ptrace and
        // reads their registers: the seccomp filter allows ptrace, and a clone with exactly the
        // flags of that process (fork, vfork, execve and every other clone stay refused), and
        // Landlock lets the process read /proc/self/task.
        bool leakCheck = false;
        // The runtime, for the detail of a layer; empty without a sanitizer
        const char *runtime = "";
    };
    SanitizerAllowances sanitizerAllowances();

#ifdef _WIN32
    // Pieces for a host that sets a child up from the outside, and what apply() uses on Windows.
    // A handle is passed as void* so that this header does not include windows.h.
    namespace windows
    {
        // A Job Object limited by the policy: memory and CPU time per process, one active
        // process, no access to the desktop, the clipboard, global atoms or the handles of
        // other processes. The caller assigns processes to it and closes it. Null on failure,
        // and *error says why.
        void *createJob(const Policy &policy, std::string *error);
        // Gives the folder (or file) a low integrity label, so that a low integrity process may
        // write in it. Must run before the process that writes is made low integrity.
        bool setLowIntegrityLabel(const std::string &pathUtf8, std::string *error);
        // Lowers the integrity level of the calling process to low
        bool lowerCurrentProcessIntegrity(std::string *error);
    } // namespace windows
#endif

} // namespace sandbox

#endif
