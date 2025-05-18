/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/cmdlist.h"
#include "include/build.h"
#include "include/init.h"
#include "include/add.h"
#include "include/remove.h"
#include "include/sync.h"
#include "include/geoproj.h"
#include "include/info.h"
#include "include/list.h"
#include "include/thumbs.h"
#include "include/setexif.h"
#include "include/login.h"
#include "include/logout.h"
#include "include/tile.h"
#include "include/share.h"
#include "include/system.h"
#include "include/status.h"
#include "include/password.h"
#include "include/delta.h"
#include "include/clone.h"
#include "include/tag.h"
#include "include/ept.h"
#include "include/push.h"
#include "include/pull.h"
#include "include/meta.h"
#include "include/stamp.h"
#include "include/cog.h"
#include "include/nxs.h"
#include "include/search.h"
#include "include/stac.h"

namespace cmd
{

  std::map<std::string, Command *> commands = {
      {"build", new Build()},
      {"init", new Init()},
      {"add", new Add()},
      {"remove", new Remove()},
      {"sync", new Sync()},
      {"geoproj", new GeoProj()},
      {"info", new Info()},
      {"list", new List()},
      {"thumbs", new Thumbs()},
      {"setexif", new SetExif()},
      {"login", new Login()},
      {"logout", new Logout()},
      {"tile", new Tile()},
      {"share", new Share()},
      {"system", new System()},
      {"status", new Status()},
      {"password", new Password()},
      {"delta", new Delta()},
      {"clone", new Clone()},
      {"tag", new Tag()},
      {"ept", new Ept()},
      {"push", new Push()},
      {"pull", new Pull()},
      {"meta", new Meta()},
      {"stamp", new Stamp()},
      {"cog", new Cog()},
      {"nxs", new Nxs()},
      {"search", new Search()},
      {"stac", new Stac()}};

  std::map<std::string, std::string> aliases = {
      {"rm", "remove"},
      {"r", "remove"},
      {"a", "add"},
      {"s", "sync"},
      {"gp", "geoproj"},
      {"i", "info"},
      {"ls", "list"},
      {"sh", "share"},
      {"psw", "password"},
      {"attrib", "chattr"},
      {"attr", "chattr"},
      {"diff", "delta"},
      {"c", "clone"},
      {"checkout", "clone"},
      {"co", "clone"},
      {"b", "build"}};

}
