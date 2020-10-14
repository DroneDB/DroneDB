/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef LIST_H
#define LIST_H

#include <map>
#include "build.h"
#include "init.h"
#include "add.h"
#include "remove.h"
#include "sync.h"
#include "geoproj.h"
#include "info.h"
#include "list.h"
#include "thumbs.h"
#include "set.h"
#include "login.h"
#include "logout.h"
#include "tile.h"
#include "share.h"
#include "system.h"

namespace cmd {

std::map<std::string, Command*> commands = {
    {"build", new Build()},
    {"init", new Init()},
    {"add", new Add()},
    {"remove", new Remove()},
    {"sync", new Sync()},
    {"geoproj", new GeoProj()},
    {"info", new Info()},
    {"list", new List()},
    {"thumbs", new Thumbs()},
    {"set", new Set()},
    {"login", new Login()},
    {"logout", new Logout()},
    {"tile", new Tile()},
    {"share", new Share()},
    {"system", new System()}
};

std::map<std::string, std::string> aliases = {
    {"rm", "remove"},
    {"r", "remove"},
    {"a", "add"},
    {"s", "sync"},
    {"gp", "geoproj"},
    {"i", "info"},
    {"ls", "list"},
    {"sh", "share"}
};

}

#endif // LIST_H
