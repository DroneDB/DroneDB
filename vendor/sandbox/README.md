# sandbox

Confinement layers for the helper programs that libddb starts to parse files a user supplied. A
parser that is exploited by such a file finds itself in a process that cannot reach the network,
cannot read other people's files, cannot start programs and cannot use more than a bounded share
of the machine.

A static library, C++17, with no dependency on libddb: every helper program links it
(`target_link_libraries(<helper> PRIVATE ddb_sandbox)`). It builds standalone as well.

```cpp
#include "sandbox/sandbox.h"

std::string warning;
sandbox::Mode mode = sandbox::parseMode(std::getenv("DDB_GDAL_SANDBOX"), &warning);

sandbox::Policy policy;
policy.readPaths = {inputFolder, gdalData, projData};
policy.writePaths = {outputFolder};
policy.maxMemoryBytes = 4ULL << 30;
policy.maxCpuSeconds = 60;

const sandbox::Report report = sandbox::apply(policy, mode);
if (!report.ok)
    return refuseToStart(report.error);      // strict, and a layer is missing
if (!report.warning().empty())
    logWarningOnce(report.warning());        // best-effort, and a layer is missing
```

`sandbox::probe()` lists the layers the host offers without restricting the process (a helper
prints it for its `--sandbox-report` option, `Report::describe()`).

## Modes

| Mode | Behaviour |
|---|---|
| `best-effort` (default, also for no value and for an unknown one) | Applies the layers the host has. `Report::warning()` names the others, to be logged once per process. |
| `strict` | Checks every layer first; when one is missing `apply()` fails, restricts nothing and names the layer in `Report::error`. The caller exits. |
| `off` | Applies no layer. The process boundary of the helper stays; this only removes the confinement of the process. |

A write path that does not exist is an error in `strict` and `best-effort` alike: it is a mistake
of the caller, not a property of the host.

## Where to call it

Once, early, in a process that was just started or forked for one job and has no other thread
yet. Every layer restricts the calling process and the threads it creates later, and none of
them can be lifted again. `apply()` does not throw and is safe in a freshly forked child.
Everything the process needs to read afterwards (GDAL and PROJ data, the program's own folder
when it loads libraries later, `/dev/null`, `/dev/urandom`, `/proc/self`) must be in
`readPaths`; the files of the job are the input folder (read) and the output folder (write).
A worker that is forked per request applies the policy of that request in the child, so the
output folder is the one of the request.

## Layers

### Linux

| Layer | What it does |
|---|---|
| `rlimits` | `RLIMIT_AS`, `RLIMIT_CPU`, `RLIMIT_FSIZE` and `RLIMIT_NOFILE` from the policy (0 means unlimited), `RLIMIT_CORE` 0 always. The CPU hard limit is one second above the soft one, so the process ends with `SIGXCPU` and not `SIGKILL`; under a hard limit the process already had (`ulimit`, a container) that is the limit it gets, and the soft one stays one second below it. |
| `oom-score` | Writes 1000 to `/proc/self/oom_score_adj`: when a container runs out of memory, the helper is killed first and not the host. |
| `no-new-privs` | `PR_SET_NO_NEW_PRIVS`: set-uid programs and file capabilities give nothing. seccomp and Landlock need it, and each sets it as well. |
| `landlock` | Read access to `readPaths`, read and write access (create, delete, rename within, truncate) to `writePaths`, nothing else. Handles every right of the Landlock ABI the kernel reports (1 to 6 are used): refer (2), truncate (3), TCP bind and connect with no rule, so none is allowed (4), ioctl on devices (5), no signals to and no abstract unix sockets of processes outside the sandbox (6). Needs a kernel 5.13 or later with Landlock in the list of security modules. |
| `seccomp` | A BPF filter written by hand. A syscall of another architecture or of the x32 ABI kills the process. `socket` and `socketpair` are refused for every family but `AF_UNIX`. `execve`, `execveat`, `ptrace`, `process_vm_readv/writev`, the pid file descriptors, `unshare`, `setns`, the mount calls, `io_uring`, `bpf`, `perf_event_open`, the key ring calls and `userfaultfd` fail with `EPERM`. No new process: `fork`, `vfork` and a `clone` without `CLONE_THREAD` or with a namespace flag fail with `EPERM`, so threads work and a child that could outlive the job with the descriptors it inherited cannot be made; `clone3` answers `ENOSYS` (its flags are in memory, where a filter cannot read them) and the C library falls back to `clone`. `setsid` and `setpgid` fail with `EPERM`: the process stays in the process group it was started in, and the host that ends that group ends it. `kill`, `tkill`, `tgkill` and the signal-queueing calls are refused unless they name the process itself. |

Why `AF_UNIX` stays: the C library uses it (logging, name service caches), and Landlock's
scoping stops abstract sockets outside the sandbox. Every other family is refused, not only the
inet ones, because netlink, packet, vsock, the crypto API and the like are kernel surface that a
parser does not need.

### macOS

`rlimits` only. The address space limit is accepted by the system and not enforced, and the
report says so. The layers of Linux are not listed, so `strict` starts and a report can show
every layer `active`.

### Windows

`job-object`: a Job Object with a memory limit and a CPU time limit per process, one active
process (the helper cannot start another) and the user interface restrictions (desktop, display
settings, exit windows, global atoms, other processes' handles, the clipboard, system
parameters). `integrity-level`: every folder of `writePaths` gets a low integrity label while
the process can still set labels, then the process drops its token to low integrity, which
stops it writing anywhere without that label. `windows::createJob`, `windows::setLowIntegrityLabel`
and `windows::lowerCurrentProcessIntegrity` are public for a host that sets a child up from the
outside. The layers of Linux are not listed, so `strict` starts and a report can show every
layer `active`.

## Residual risks

* Landlock and seccomp need a kernel that has them. Docker's default seccomp profile and some
  hosted environments hide Landlock; `best-effort` then runs with the layers that remain and
  says so once, `strict` refuses to start.
* The filter does not limit what `AF_UNIX` sockets can reach: a path socket is reached through
  file permissions, which the Landlock ABIs used here do not cover. Abstract sockets are scoped
  from ABI 6.
* Landlock before ABI 4 does not cover TCP, and no ABI covers UDP; the seccomp filter does both.
* Before Landlock ABI 6 a process can signal its own user's processes; seccomp restricts
  signals to the process itself on every kernel that has it.
* The address space limit counts reserved memory, not used memory, so a limit has to leave room
  for the mappings of the libraries the process loads.
* Reads on Windows and macOS are not confined and neither is the network; an exploit there has
  the rights of the user running the helper, but never the memory of the host.
* The Windows code builds with MSVC but has not been run by the Linux development builds of
  this tree; it needs a run on Windows before it is relied on.
* The layers are not a replacement for the process boundary: a helper that fails still has to be
  treated as untrusted, and its output validated by the host.
