/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The Windows layers: a Job Object that limits memory, CPU time and the number of processes
// and shuts the process off from the desktop, and a low integrity level, which keeps the process
// from writing anywhere that has not been given a low integrity label (the output folders of
// the policy, which the host labels). Reads and the network are not confined on Windows. The
// layers of Linux are not listed: a layer is missing only where its system could offer it, so
// strict can start here and the report of a host can show every layer active.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <memory>
#include <string>

#include "layers.h"

namespace sandbox
{
    namespace
    {
        std::string lastError(const char *what)
        {
            const DWORD code = GetLastError();
            return std::string(what) + " failed (error " + std::to_string(code) + ")";
        }

        std::wstring widen(const std::string &utf8)
        {
            if (utf8.empty())
                return std::wstring();
            const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (length <= 0)
                return std::wstring();
            std::wstring wide(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], length);
            return wide;
        }

        struct LocalFreeDeleter
        {
            void operator()(void *memory) const
            {
                LocalFree(memory);
            }
        };
        using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

        struct HandleCloser
        {
            void operator()(void *handle) const
            {
                CloseHandle(handle);
            }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;
    } // namespace

    namespace windows
    {
        void *createJob(const Policy &policy, std::string *error)
        {
            Handle job(CreateJobObjectW(nullptr, nullptr));
            if (!job)
            {
                *error = lastError("CreateJobObject");
                return nullptr;
            }

            // Limits per process: memory and user-mode CPU time (in 100 ns units). One active
            // process, so the process cannot start another; an exception nobody handles ends the
            // process instead of showing a dialog.
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            limits.BasicLimitInformation.ActiveProcessLimit = 1;
            if (policy.maxMemoryBytes != 0)
            {
                limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
                limits.ProcessMemoryLimit = static_cast<SIZE_T>(policy.maxMemoryBytes);
            }
            if (policy.maxCpuSeconds != 0)
            {
                limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
                limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
                    static_cast<LONGLONG>(policy.maxCpuSeconds) * 10000000LL;
            }
            if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            {
                *error = lastError("SetInformationJobObject(limits)");
                return nullptr;
            }

            JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
            ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                                     JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                     JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                     JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
            if (!SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
            {
                *error = lastError("SetInformationJobObject(ui)");
                return nullptr;
            }
            return job.release();
        }

        bool setLowIntegrityLabel(const std::string &pathUtf8, std::string *error)
        {
            // A mandatory label that lets low integrity processes write ("NW" is no-write-up
            // cleared by the low level) and that folders and files below inherit
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1,
                                                                      &descriptor, nullptr))
            {
                *error = lastError("ConvertStringSecurityDescriptorToSecurityDescriptor");
                return false;
            }
            const LocalMemory owner(descriptor);

            BOOL saclPresent = FALSE;
            BOOL saclDefaulted = FALSE;
            PACL sacl = nullptr;
            if (!GetSecurityDescriptorSacl(descriptor, &saclPresent, &sacl, &saclDefaulted) || !saclPresent)
            {
                *error = lastError("GetSecurityDescriptorSacl");
                return false;
            }

            std::wstring path = widen(pathUtf8);
            const DWORD status = SetNamedSecurityInfoW(&path[0], SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr,
                                                       nullptr, nullptr, sacl);
            if (status != ERROR_SUCCESS)
            {
                *error = "SetNamedSecurityInfo failed for '" + pathUtf8 + "' (error " + std::to_string(status) + ")";
                return false;
            }
            return true;
        }

        bool hasLowIntegrityLabel(const std::string &pathUtf8, bool *low, std::string *error)
        {
            *low = false;
            PACL sacl = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            const std::wstring path = widen(pathUtf8);
            const DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
                                                       nullptr, nullptr, nullptr, &sacl, &descriptor);
            if (status != ERROR_SUCCESS)
            {
                *error = "GetNamedSecurityInfo failed for '" + pathUtf8 + "' (error " + std::to_string(status) + ")";
                return false;
            }
            const LocalMemory owner(descriptor);

            // No label is the medium level. A label that is only inherited by what is created
            // below (inherit-only) is not the object's own.
            if (sacl == nullptr)
                return true;
            for (DWORD i = 0; i < sacl->AceCount; i++)
            {
                void *ace = nullptr;
                if (!GetAce(sacl, i, &ace))
                {
                    *error = lastError("GetAce");
                    return false;
                }
                const auto *header = static_cast<const ACE_HEADER *>(ace);
                if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE ||
                    (header->AceFlags & INHERIT_ONLY_ACE) != 0)
                    continue;
                auto *label = static_cast<SYSTEM_MANDATORY_LABEL_ACE *>(ace);
                const PSID sid = &label->SidStart;
                const UCHAR count = *GetSidSubAuthorityCount(sid);
                if (count == 0)
                    continue;
                const DWORD level = *GetSidSubAuthority(sid, static_cast<DWORD>(count - 1));
                *low = level <= static_cast<DWORD>(SECURITY_MANDATORY_LOW_RID);
                return true;
            }
            return true;
        }

        bool lowerCurrentProcessIntegrity(std::string *error)
        {
            HANDLE rawToken = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &rawToken))
            {
                *error = lastError("OpenProcessToken");
                return false;
            }
            const Handle token(rawToken);

            // S-1-16-4096: the low mandatory level. A token can be lowered, never raised.
            PSID lowSid = nullptr;
            if (!ConvertStringSidToSidW(L"S-1-16-4096", &lowSid))
            {
                *error = lastError("ConvertStringSidToSid");
                return false;
            }
            const LocalMemory owner(lowSid);

            TOKEN_MANDATORY_LABEL label{};
            label.Label.Attributes = SE_GROUP_INTEGRITY;
            label.Label.Sid = lowSid;
            if (!SetTokenInformation(token.get(), TokenIntegrityLevel, &label,
                                     sizeof(label) + GetLengthSid(lowSid)))
            {
                *error = lastError("SetTokenInformation(TokenIntegrityLevel)");
                return false;
            }
            return true;
        }
    } // namespace windows

    namespace detail
    {
        namespace
        {
            // The calling process joins a new job. The job handle stays open for the life of the
            // process: closing it would not lift the limits, there is just nothing to gain.
            LayerResult applyJobObject(const Policy &policy)
            {
                std::string error;
                void *job = windows::createJob(policy, &error);
                if (job == nullptr)
                    return result("job-object", false, error);
                if (!AssignProcessToJobObject(job, GetCurrentProcess()))
                {
                    const std::string failure = lastError("AssignProcessToJobObject");
                    CloseHandle(job);
                    return result("job-object", false, failure);
                }
                return result("job-object", true, "memory, CPU time, one process, no access to the desktop");
            }

            LayerState probeJobObject()
            {
                return state("job-object", true);
            }

            // The process drops to low integrity, so it writes only below the output folders,
            // which must have a low integrity label already: the host gives one to the folder it
            // makes for a request while the folder is empty. The label is not given here: Windows
            // carries a new label of a folder down to the files already in it, and a program
            // among them would start at low integrity from then on, and could no longer write
            // in its own folders. An output folder without the label is refused, and keeps the
            // label it has.
            LayerResult applyIntegrity(const Policy &policy)
            {
                std::string error;
                for (const auto &path : policy.writePaths)
                {
                    bool low = false;
                    if (!windows::hasLowIntegrityLabel(path, &low, &error))
                        return fatal("integrity-level", error);
                    if (!low)
                        return fatal("integrity-level",
                                     "the output folder '" + path +
                                         "' has no low integrity label: the host gives it one when it "
                                         "makes the folder (sandbox::windows::setLowIntegrityLabel)");
                }
                if (!windows::lowerCurrentProcessIntegrity(&error))
                    return result("integrity-level", false, error);
                return result("integrity-level", true, "writes only below folders with a low integrity label");
            }

            LayerState probeIntegrity()
            {
                return state("integrity-level", true);
            }
        } // namespace

        const std::vector<Layer> &platformLayers()
        {
            static const std::vector<Layer> layers = {
                {"job-object", applyJobObject, probeJobObject},
                {"integrity-level", applyIntegrity, probeIntegrity},
            };
            return layers;
        }
    } // namespace detail
} // namespace sandbox
