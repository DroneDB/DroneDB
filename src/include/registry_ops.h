/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGISTRY_OPS_H
#define REGISTRY_OPS_H

// What a dataset does with a Registry: pull, push and clone (DQ-11: out of the Registry class,
// one unit each). The *Dataset functions work over a RegistryClient that is logged in; push,
// pull and clone are what the CLI runs from the dataset folder: they find the Registry, log in
// through withAuthenticatedRegistry (registry_auth.h) and ask `credentials` when they must.

#include <iostream>
#include <string>

#include "database.h"
#include "ddb_export.h"
#include "delta_apply.h"
#include "registry_auth.h"
#include "registry_client.h"
#include "registryutils.h"

namespace ddb {

// Pulls the dataset at `path` from the Registry of its tag, applying the changes with
// `mergeStrategy`. Throws IndexException when the dataset has no tag.
DDB_DLL void pullDataset(RegistryClient &client, const std::string &path, MergeStrategy mergeStrategy,
                         std::ostream &out);

// Pushes the dataset at `path` to the Registry of its tag: init, the metadata, the files the
// server needs, commit. Throws IndexException when the dataset has no tag and
// PullRequiredException when the Registry has changes the dataset has not pulled. Nothing in
// the sequence is tried again; an error ends the push before the commit.
DDB_DLL void pushDataset(RegistryClient &client, const std::string &path, std::ostream &out);

// Clones the dataset into `folder` (empty or missing): indexes it, tags it and pulls it
DDB_DLL void cloneDataset(RegistryClient &client, const std::string &organization, const std::string &dataset,
                          const std::string &folder, std::ostream &out);

// The Registry of the dataset at `db`: `registry` when given, else the one in its tag.
// `verb` ("push", "pull") names the operation in the error of a dataset without a tag.
DDB_DLL std::string datasetRegistryUrl(Database *db, const std::string &registry, const std::string &verb);

// The operations of the CLI, from the current folder. `registry` may be empty (the tag says).
// `deps` are the client's collaborators (RegistryDeps): the defaults are the real ones.
DDB_DLL void push(const std::string &registry, bool sslVerify, CredentialProvider &credentials,
                  const RegistryDeps &deps = {});
DDB_DLL void pull(const std::string &registry, MergeStrategy mergeStrategy, bool sslVerify,
                  CredentialProvider &credentials, const RegistryDeps &deps = {});
DDB_DLL void clone(const TagComponents &tag, const std::string &folder, bool sslVerify,
                   CredentialProvider &credentials, const RegistryDeps &deps = {});

} // namespace ddb

#endif // REGISTRY_OPS_H
