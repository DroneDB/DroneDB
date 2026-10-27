/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/cmdlist.h"

#include <stdexcept>

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
#include "include/bm3d.h"
#include "include/tiles3d.h"
#include "include/gsplat.h"
#include "include/search.h"
#include "include/stac.h"
#include "include/mask.h"
#include "include/merge_multispectral.h"
#include "include/cleanup.h"

namespace cmd
{

  const CommandRegistry &CommandRegistry::builtin()
  {
    static const CommandRegistry registry = [] {
      CommandRegistry r;
      r.add<Build>("build");
      r.add<Init>("init");
      r.add<Add>("add");
      r.add<Align>("align");
      r.add<Remove>("remove");
      r.add<Sync>("sync");
      r.add<Rescan>("rescan");
      r.add<GeoProj>("geoproj");
      r.add<Info>("info");
      r.add<List>("list");
      r.add<Thumbs>("thumbs");
      r.add<SetExif>("setexif");
      r.add<Login>("login");
      r.add<Logout>("logout");
      r.add<Tile>("tile");
      r.add<Share>("share");
      r.add<System>("system");
      r.add<Status>("status");
      r.add<Password>("password");
      r.add<Delta>("delta");
      r.add<Clone>("clone");
      r.add<Tag>("tag");
      r.add<Copc>("copc");
      r.add<Push>("push");
      r.add<Pull>("pull");
      r.add<Meta>("meta");
      r.add<Stamp>("stamp");
      r.add<Cog>("cog");
      r.add<Contour>("contour");
      r.add<Bm3d>("bm3d");
      r.add<Tiles3d>("3dtiles");
      r.add<Gsplat>("gsplat");
      r.add<Search>("search");
      r.add<Stac>("stac");
      r.add<Mask>("mask");
      r.add<MergeMultispectral>("merge-multispectral");
      r.add<Cleanup>("cleanup");

      r.alias("rm", "remove");
      r.alias("r", "remove");
      r.alias("a", "add");
      r.alias("s", "sync");
      r.alias("rs", "rescan");
      r.alias("gp", "geoproj");
      r.alias("i", "info");
      r.alias("ls", "list");
      r.alias("sh", "share");
      r.alias("psw", "password");
      r.alias("diff", "delta");
      r.alias("c", "clone");
      r.alias("checkout", "clone");
      r.alias("co", "clone");
      r.alias("b", "build");
      return r;
    }();
    return registry;
  }

  void CommandRegistry::add(const std::string &name, Factory factory)
  {
    if (!factories_.emplace(name, std::move(factory)).second)
      throw std::logic_error("Command registered twice: " + name);
  }

  void CommandRegistry::alias(const std::string &alias, const std::string &target)
  {
    if (factories_.count(target) == 0)
      throw std::logic_error("Alias " + alias + " names a command that is not registered: " + target);
    if (factories_.count(alias) != 0 || !aliases_.emplace(alias, target).second)
      throw std::logic_error("Alias is already a command or an alias: " + alias);
  }

  std::string CommandRegistry::resolve(const std::string &name) const
  {
    if (factories_.count(name) != 0)
      return name;
    const auto it = aliases_.find(name);
    return it == aliases_.end() ? std::string() : it->second;
  }

  std::unique_ptr<Command> CommandRegistry::create(const std::string &name) const
  {
    const auto resolved = resolve(name);
    return resolved.empty() ? nullptr : factories_.at(resolved)();
  }

  std::vector<std::string> CommandRegistry::names() const
  {
    std::vector<std::string> result;
    for (const auto &entry : factories_)
      result.push_back(entry.first);
    return result;
  }

}
