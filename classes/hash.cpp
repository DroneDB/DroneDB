/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include "hash.h"
#include "exceptions.h"

std::string Hash::ingest(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw FSException("Cannot open " + path + " for hashing");
    }

    const size_t BufferSize = 144*7*1024;
    char* buffer = new char[BufferSize];
    SHA256 digestSha2;

    while (f) {
        f.read(buffer, BufferSize);
        size_t numBytesRead = size_t(f.gcount());
        digestSha2.add(buffer, numBytesRead);
    }

    f.close();
    delete[] buffer;

    return digestSha2.getHash();
}


