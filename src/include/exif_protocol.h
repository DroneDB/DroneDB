/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIF_PROTOCOL_H
#define EXIF_PROTOCOL_H

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "exif_metadata.h"
#include "fs.h"

namespace ddb
{
    namespace exif
    {
        // The client side of the protocol of metadata-util (vendor/metadata-util/PROTOCOL.md): the lines
        // libddb sends and how it understands the lines that come back. No process, no pipe
        // here: exif_helper_pool.h runs them.
        namespace protocol
        {
            // The version this client implements. A helper is accepted when its major version is
            // the same and its minor version is not lower.
            constexpr int kMajorVersion = 1;
            constexpr int kMinorVersion = 0;

            struct Version
            {
                int major = 0;
                int minor = 0;
            };

            // The version a helper reports on `--version`: the line ends with "protocol
            // <major>.<minor>", whatever precedes it. Empty when no line does.
            std::optional<Version> parseVersionOutput(std::string_view output);
            bool isAccepted(const Version &helper);
            std::string toString(const Version &version);

            enum class Operation
            {
                Read,
                Write
            };

            // A line that is not what the protocol says (invalid JSON, a member missing or of the
            // wrong type, text that is not base64): a broken helper
            class ProtocolError : public std::runtime_error
            {
              public:
                using std::runtime_error::runtime_error;
            };

            // The request lines (without the line feed). The path is sent absolute, resolved
            // against the working directory of this process now (the helper's is the one it
            // started in). A path or a value that is not valid UTF-8 is sent as base64 in its
            // own member (path64, value64).
            // @throws ProtocolError when a key to set is not valid UTF-8 (they are ASCII), or a
            // relative path cannot be made absolute (the working directory is gone)
            std::string readRequest(int64_t id, const fs::path &file);
            std::string writeRequest(int64_t id, const fs::path &file, const TagAssignments &set);

            // Whether the error member of a failure response says the file has no metadata the
            // helper can read (it cannot be opened, is not an image, is damaged), as opposed to a
            // helper that rejects the request or reports an error this client does not know
            bool isFileError(std::string_view error);

            struct Response
            {
                // Empty when the helper could not read the id of the request
                std::optional<int64_t> id;
                bool ok = false;

                // ok == false
                std::string error;
                std::string message;

                // ok, for a read
                ImageMetadata metadata;
                // ok, for a write
                std::vector<Tag> tags;
            };

            // @throws ProtocolError
            Response parseResponse(std::string_view line, Operation operation);

        } // namespace protocol
    } // namespace exif
} // namespace ddb

#endif // EXIF_PROTOCOL_H
