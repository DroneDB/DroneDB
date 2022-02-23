/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef DATABASE_H
#define DATABASE_H

#define DDB_BUILD_PATH "build"

#include "metamanager.h"
#include "sqlite_database.h"
#include "ddb_export.h"
#include "json.h"
#include "constants.h"

namespace ddb{

class Database : public SqliteDatabase {
  private:
    MetaManager *metaManager = nullptr;
  protected:
    void setIntAttribute(const std::string &name, long value);
    void setLongAttribute(const std::string &name, long long value);

    void setBoolAttribute(const std::string &name, bool value);

    int getIntAttribute(const std::string &name) const;
    long long getLongAttribute(const std::string &name) const;
    bool getBoolAttribute(const std::string &name) const;

    bool hasAttribute(const std::string &name) const;
    void clearAttribute(const std::string &name);

  public:
      DDB_DLL ~Database();
      DDB_DLL static void Initialize();
      DDB_DLL void afterOpen() override;
      DDB_DLL Database &createTables();
      DDB_DLL void ensureSchemaConsistency();

      DDB_DLL void setPublic(bool isPublic);
      DDB_DLL bool isPublic() const;

      DDB_DLL void chattr(json attrs);

      DDB_DLL json getAttributes() const;
      DDB_DLL json getStamp() const;

      DDB_DLL fs::path rootDirectory() const;
      DDB_DLL fs::path ddbDirectory() const;
      DDB_DLL fs::path tmpDirectory() const;
      DDB_DLL fs::path buildDirectory() const;

      DDB_DLL MetaManager* getMetaManager();
};

}

#endif // DATABASE_H
