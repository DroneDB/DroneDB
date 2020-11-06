/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "cmdlist.h"

#include "add.h"
#include "build.h"
#include "geoproj.h"
#include "info.h"
#include "init.h"
#include "list.h"
#include "login.h"
#include "logout.h"
#include "password.h"
#include "remove.h"
#include "setexif.h"
#include "share.h"
#include "status.h"
#include "sync.h"
#include "system.h"
#include "thumbs.h"
#include "tile.h"

namespace cmd {

std::map<std::string, Command*> commands = {
    {"build", new Build()},      {"init", new Init()},
    {"add", new Add()},          {"remove", new Remove()},
    {"sync", new Sync()},        {"geoproj", new GeoProj()},
    {"info", new Info()},        {"list", new List()},
    {"thumbs", new Thumbs()},    {"setexif", new SetExif()},
    {"login", new Login()},      {"logout", new Logout()},
    {"tile", new Tile()},        {"share", new Share()},
    {"system", new System()},    {"status", new Status()},
    {"password", new Password()}};

std::map<std::string, std::string> aliases = {
    {"rm", "remove"}, {"r", "remove"},   {"a", "add"},
    {"s", "sync"},    {"gp", "geoproj"}, {"i", "info"},
    {"ls", "list"},   {"sh", "share"},   {"psw", "password"}};

}  // namespace cmd
