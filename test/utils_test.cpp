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

#include "gtest/gtest.h"
#include "../utils.h"
#include <vector>
#include <string>

namespace {

TEST(PathsAreChildren, Normal) {
    EXPECT_TRUE(utils::pathsAreChildren("/my/path", {"/my/path/1", "/my/path"}));
    EXPECT_TRUE(utils::pathsAreChildren("path", {"path/1/2", "path/3", "path"}));
    EXPECT_FALSE(utils::pathsAreChildren("path", {"test", "path/3", "path"}));
    EXPECT_FALSE(utils::pathsAreChildren("/my/path", {"/my/pat", "/my/path/1"}));
}

}
