/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/cmdlist.h"
#include "include/build.h"
#include "include/init.h"
#include "include/add.h"
#include "include/align.h"
#include "include/remove.h"
#include "include/sync.h"
#include "include/rescan.h"
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
#include "include/copc.h"
#include "include/push.h"
#include "include/pull.h"
#include "include/meta.h"
#include "include/stamp.h"
#include "include/cog.h"
#include "include/contour.h"
#include "include/nxs.h"
#include "include/tiles3d.h"
#include "include/gsplat.h"
#include "include/search.h"
#include "include/stac.h"
#include "include/mask.h"
#include "include/merge_multispectral.h"
#include "include/cleanup.h"

namespace cmd
{

  namespace
  {
    // Commands are stateless and live as long as the process: one object with static
    // storage per command type, destroyed at exit, instead of a heap object that was never
    // freed (LeakSanitizer, DQ-01)
    template <typename T>
    Command *instance()
    {
      static T command;
      return &command;
    }
  }

  std::map<std::string, Command *> commands = {
      {"build", instance<Build>()},
      {"init", instance<Init>()},
      {"add", instance<Add>()},
      {"align", instance<Align>()},
      {"remove", instance<Remove>()},
      {"sync", instance<Sync>()},
      {"rescan", instance<Rescan>()},
      {"geoproj", instance<GeoProj>()},
      {"info", instance<Info>()},
      {"list", instance<List>()},
      {"thumbs", instance<Thumbs>()},
      {"setexif", instance<SetExif>()},
      {"login", instance<Login>()},
      {"logout", instance<Logout>()},
      {"tile", instance<Tile>()},
      {"share", instance<Share>()},
      {"system", instance<System>()},
      {"status", instance<Status>()},
      {"password", instance<Password>()},
      {"delta", instance<Delta>()},
      {"clone", instance<Clone>()},
      {"tag", instance<Tag>()},
      {"copc", instance<Copc>()},
      {"push", instance<Push>()},
      {"pull", instance<Pull>()},
      {"meta", instance<Meta>()},
      {"stamp", instance<Stamp>()},
      {"cog", instance<Cog>()},
      {"contour", instance<Contour>()},
      {"nxs", instance<Nxs>()},
      {"3dtiles", instance<Tiles3d>()},
      {"gsplat", instance<Gsplat>()},
      {"search", instance<Search>()},
      {"stac", instance<Stac>()},
      {"mask", instance<Mask>()},
      {"merge-multispectral", instance<MergeMultispectral>()},
      {"cleanup", instance<Cleanup>()}};

  std::map<std::string, std::string> aliases = {
      {"rm", "remove"},
      {"r", "remove"},
      {"a", "add"},
      {"s", "sync"},
      {"rs", "rescan"},
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
