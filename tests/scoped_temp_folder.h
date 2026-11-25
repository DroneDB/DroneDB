/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DDB_TEST_SCOPED_TEMP_FOLDER_H
#define DDB_TEST_SCOPED_TEMP_FOLDER_H

#include <cstdlib>
#include <optional>
#include <string>

#include "fs.h"
#include "mio.h"

namespace testutils
{
    // While it lives, the temporary folder of the system (fs::temp_directory_path) is `folder`: the variable
    // it reads (TMPDIR; TMP on Windows) is set, and put back as it was when it ends
    class ScopedTempFolder
    {
      public:
        explicit ScopedTempFolder(const fs::path &folder) : previous_(ddb::io::getEnvUtf8(kName))
        {
            set(ddb::io::toUtf8(folder));
        }
        ~ScopedTempFolder() { set(previous_.value_or("")); }
        ScopedTempFolder(const ScopedTempFolder &) = delete;
        ScopedTempFolder &operator=(const ScopedTempFolder &) = delete;

      private:
#ifdef _WIN32
        static constexpr const char *kName = "TMP";
        static void set(const std::string &value)
        {
            _wputenv_s(ddb::io::toWide(kName).c_str(), ddb::io::toWide(value).c_str());
        }
#else
        static constexpr const char *kName = "TMPDIR";
        static void set(const std::string &value)
        {
            if (value.empty())
                unsetenv(kName);
            else
                setenv(kName, value.c_str(), 1);
        }
#endif
        std::optional<std::string> previous_;
    };
} // namespace testutils

#endif // DDB_TEST_SCOPED_TEMP_FOLDER_H
