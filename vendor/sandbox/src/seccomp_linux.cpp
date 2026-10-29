/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The seccomp-bpf filter. It is written as raw BPF, so the library needs nothing but the kernel
// headers, and it is an allow list by exception: everything a parser needs stays allowed, and
// what an exploit needs to get out of the process or to reach the network is refused.
//
//   * A syscall of another architecture, or of the x32 ABI, kills the process: a native
//     parser never makes one, and the filter below would not know their numbers.
//   * socket and socketpair are refused for every family but AF_UNIX. The inet families are
//     the network; the others (netlink, packet, vsock, the crypto API, ...) are kernel surface a
//     parser has no use for. AF_UNIX stays, since C libraries use it for logging and name
//     lookups.
//   * Namespaces, mounts, tracing of other processes, io_uring, bpf, perf, the key ring,
//     userfaultfd and the pid file descriptors are refused with EPERM; so is execve, so a
//     compromised parser cannot start a program.
//   * No new process: fork and vfork are refused with EPERM, and so is a clone that is not a
//     thread of this process (no CLONE_THREAD) or that asks for a namespace. Threads work. A
//     child of a compromised parser would outlive the request: it could keep descriptors the
//     process inherited and go on writing to them after the process ended. clone3 passes its
//     flags in memory, where a filter cannot read them: it answers ENOSYS, and the C library
//     falls back to clone.
//   * setsid and setpgid are refused with EPERM: the process stays in the process group it was
//     started in, so whoever ends that group (the host of a helper kills the group of the
//     helper) ends the process too, whatever it did to its own parent-death signal.
//   * kill, tkill, tgkill and the signal-queueing calls are refused unless they name the process
//     itself, so the host is never signalled.

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "layers.h"

// Syscalls newer than the oldest kernel headers we build against. Their numbers are the same on
// every architecture from 424 on.
#ifndef __NR_pidfd_send_signal
#define __NR_pidfd_send_signal 424
#endif
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#define __NR_io_uring_enter 426
#define __NR_io_uring_register 427
#endif
#ifndef __NR_open_tree
#define __NR_open_tree 428
#define __NR_move_mount 429
#define __NR_fsopen 430
#define __NR_fsconfig 431
#define __NR_fsmount 432
#define __NR_fspick 433
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif
#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif

#if defined(__x86_64__)
#define SANDBOX_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SANDBOX_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif

namespace sandbox::detail
{
#ifdef SANDBOX_AUDIT_ARCH
    namespace
    {
        constexpr uint16_t Load = BPF_LD | BPF_W | BPF_ABS;
        constexpr uint16_t Equal = BPF_JMP | BPF_JEQ | BPF_K;
        constexpr uint16_t AnyBit = BPF_JMP | BPF_JSET | BPF_K;
        constexpr uint16_t Return = BPF_RET | BPF_K;

        constexpr uint32_t Allow = SECCOMP_RET_ALLOW;
        constexpr uint32_t Kill = SECCOMP_RET_KILL_PROCESS;
        constexpr uint32_t errorNumber(int number)
        {
            return SECCOMP_RET_ERRNO | (static_cast<uint32_t>(number) & SECCOMP_RET_DATA);
        }

        // The namespaces a clone can ask for
        constexpr uint32_t NamespaceFlags = static_cast<uint32_t>(CLONE_NEWNS) | CLONE_NEWCGROUP | CLONE_NEWUTS |
                                            CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET;

        // Where in seccomp_data the first 32 bits of an argument are (the arguments the filter
        // reads are all 32 bit values: a family, a pid, the flags of clone)
        constexpr uint32_t argumentOffset(unsigned index)
        {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return static_cast<uint32_t>(offsetof(seccomp_data, args)) + 8 * index;
#else
            return static_cast<uint32_t>(offsetof(seccomp_data, args)) + 8 * index + 4;
#endif
        }

        sock_filter statement(uint16_t code, uint32_t k)
        {
            return sock_filter{code, 0, 0, k};
        }
        sock_filter jump(uint16_t code, uint32_t k, uint8_t ifTrue, uint8_t ifFalse)
        {
            return sock_filter{code, ifTrue, ifFalse, k};
        }

        // Appends the rules in order. Every rule that matches a syscall ends in a verdict, so a
        // rule that does not match leaves the syscall number in the accumulator for the next.
        class Filter
        {
          public:
            Filter()
            {
                // A foreign architecture or the x32 ABI kills the process
                add(statement(Load, static_cast<uint32_t>(offsetof(seccomp_data, arch))));
                add(jump(Equal, SANDBOX_AUDIT_ARCH, 1, 0));
                add(statement(Return, Kill));
                add(statement(Load, static_cast<uint32_t>(offsetof(seccomp_data, nr))));
#ifdef __x86_64__
                add(jump(AnyBit, 0x40000000U /* __X32_SYSCALL_BIT */, 0, 1));
                add(statement(Return, Kill));
#endif
            }

            // The syscall fails with `error`
            void deny(long number, int error)
            {
                add(jump(Equal, static_cast<uint32_t>(number), 0, 1));
                add(statement(Return, errorNumber(error)));
            }

            // The syscall is allowed when its first argument is `value`
            void allowWhenArgumentEquals(long number, unsigned argument, uint32_t value)
            {
                rule(number, {statement(Load, argumentOffset(argument)), jump(Equal, value, 1, 0),
                              statement(Return, errorNumber(EPERM)), statement(Return, Allow)});
            }

            // The syscall is allowed when the argument has `required` set and none of
            // `forbidden`; it fails with EPERM otherwise
            void allowWhenArgumentHasBitAndNone(long number, unsigned argument, uint32_t required, uint32_t forbidden)
            {
                rule(number, {statement(Load, argumentOffset(argument)), jump(AnyBit, forbidden, 1, 0),
                              jump(AnyBit, required, 1, 0), statement(Return, errorNumber(EPERM)),
                              statement(Return, Allow)});
            }

            sock_fprog finish()
            {
                add(statement(Return, Allow));
                return sock_fprog{static_cast<unsigned short>(code_.size()), code_.data()};
            }

          private:
            void add(const sock_filter &instruction)
            {
                code_.push_back(instruction);
            }
            void rule(long number, std::initializer_list<sock_filter> body)
            {
                add(jump(Equal, static_cast<uint32_t>(number), 0, static_cast<uint8_t>(body.size())));
                for (const auto &instruction : body)
                    add(instruction);
            }

            std::vector<sock_filter> code_;
        };

        // Builds the program for the process `self`
        Filter buildFilter(uint32_t self)
        {
            Filter filter;

            // Network
            filter.allowWhenArgumentEquals(__NR_socket, 0, AF_UNIX);
            filter.allowWhenArgumentEquals(__NR_socketpair, 0, AF_UNIX);
            filter.deny(__NR_io_uring_setup, EPERM);
            filter.deny(__NR_io_uring_enter, EPERM);
            filter.deny(__NR_io_uring_register, EPERM);

            // Other processes: no tracing, no memory access, no signals, no pid file descriptors
            filter.deny(__NR_ptrace, EPERM);
            filter.deny(__NR_process_vm_readv, EPERM);
            filter.deny(__NR_process_vm_writev, EPERM);
            filter.deny(__NR_pidfd_open, EPERM);
            filter.deny(__NR_pidfd_getfd, EPERM);
            filter.deny(__NR_pidfd_send_signal, EPERM);
            filter.allowWhenArgumentEquals(__NR_kill, 0, self);
            filter.allowWhenArgumentEquals(__NR_tkill, 0, self);
            filter.allowWhenArgumentEquals(__NR_tgkill, 0, self);
            filter.allowWhenArgumentEquals(__NR_rt_sigqueueinfo, 0, self);
            filter.allowWhenArgumentEquals(__NR_rt_tgsigqueueinfo, 0, self);

            // Programs
            filter.deny(__NR_execve, EPERM);
            filter.deny(__NR_execveat, EPERM);

            // Processes: a thread of this process, never another process or a namespace, and
            // no way out of the process group
            filter.allowWhenArgumentHasBitAndNone(__NR_clone, 0, CLONE_THREAD, NamespaceFlags);
            filter.deny(__NR_clone3, ENOSYS);
#ifdef __NR_fork
            filter.deny(__NR_fork, EPERM);
#endif
#ifdef __NR_vfork
            filter.deny(__NR_vfork, EPERM);
#endif
            filter.deny(__NR_setsid, EPERM);
            filter.deny(__NR_setpgid, EPERM);

            // Namespaces and mounts
            filter.deny(__NR_unshare, EPERM);
            filter.deny(__NR_setns, EPERM);
            filter.deny(__NR_mount, EPERM);
            filter.deny(__NR_umount2, EPERM);
            filter.deny(__NR_pivot_root, EPERM);
            filter.deny(__NR_open_tree, EPERM);
            filter.deny(__NR_move_mount, EPERM);
            filter.deny(__NR_fsopen, EPERM);
            filter.deny(__NR_fsconfig, EPERM);
            filter.deny(__NR_fsmount, EPERM);
            filter.deny(__NR_fspick, EPERM);
            filter.deny(__NR_mount_setattr, EPERM);

            // Kernel interfaces that are attack surface and that a parser does not use
            filter.deny(__NR_bpf, EPERM);
            filter.deny(__NR_perf_event_open, EPERM);
            filter.deny(__NR_keyctl, EPERM);
            filter.deny(__NR_add_key, EPERM);
            filter.deny(__NR_request_key, EPERM);
            filter.deny(__NR_userfaultfd, EPERM);
            return filter;
        }
    } // namespace

    LayerState probeSeccomp()
    {
        uint32_t action = SECCOMP_RET_ERRNO;
        if (syscall(__NR_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0)
            return state("seccomp", true);
        if (errno == ENOSYS && prctl(PR_GET_SECCOMP) >= 0)
            return state("seccomp", true);
        return state("seccomp", false, std::string("not available: ") + std::strerror(errno));
    }

    LayerResult applySeccomp(const Policy &)
    {
        // The filter must not be installed by a process that could still gain privileges, and
        // the process id the filter lets signals through to is the one of this process
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
            return result("seccomp", false, std::string("no_new_privs: ") + std::strerror(errno));

        Filter filter = buildFilter(static_cast<uint32_t>(getpid()));
        const sock_fprog program = filter.finish();
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
            return result("seccomp", false, std::string("not installed: ") + std::strerror(errno));
        return result("seccomp", true, "no sockets but AF_UNIX, no exec, no new process, no namespaces, no signals to other processes");
    }
#else
    LayerState probeSeccomp()
    {
        return state("seccomp", false, "architecture not supported");
    }

    LayerResult applySeccomp(const Policy &)
    {
        return result("seccomp", false, "architecture not supported");
    }
#endif
} // namespace sandbox::detail
