# libddb contract fixtures

The JSON outputs of libddb that Registry consumes, recorded as golden fixtures with a JSON
Schema each. They freeze today's field names, types and values: any unplanned change to one of
these outputs fails a DroneDB test, and Registry's contract tests (`FakeDdbWrapper`) read
the same files.

Registry validates the contract on its side, against a libddb build (its `Registry.Adapters.Ddb.Test`
suite runs against the pinned libddb in Registry's CI, and a nightly job runs it against the
latest DroneDB artifacts). DroneDB's CI does not run Registry: it tests its own documented output,
which is what the tests of this folder do.

The fixtures record the outputs **as they are today** (libddb 1.12.2, ABI 1.0). Nothing was
renamed or retyped for them, including the mixed casing:

* `point_geom` and `polygon_geom` are snake_case; the entry `properties` keys (`captureTime`,
  `bandCount`, ...) are camelCase; STAC keeps its own `stac_version` and `stac_extensions`.
* `mtime` (entries and meta items), `lastAttempt` and `retryAfter` are integer Unix epoch **seconds**.
* An image's `captureTime` is epoch **milliseconds**, written as a float (`1714972089000.0`).
* `DDBInfo` writes `path` as `file://` + the absolute path, and `depth` as the number of folders
  in that absolute path. `DDBGet`/`DDBList` write dataset-relative paths and depths.
* `DDBInfo` keeps full coordinate precision; entries read back from the index (`DDBGet`,
  `DDBList`) have point coordinates rounded to 6 decimals.
* `DDBMetaGet`/`DDBMetaAdd` return `data` as a JSON value; `DDBMetaDump` returns it as JSON text.
* `DDBGetRasterInfo` writes `"detectedSensor": null` for an unknown sensor,
  `DDBGetRasterMetadata` writes `""`.

## Layout

| Path | Content |
| --- | --- |
| `inputs/` | The committed input files (byte for byte, see `.gitattributes`) |
| `schemas/` | JSON Schemas (draft 2020-12), one per output shape |
| `fixtures/` | One golden file per output: `<name>.json` |
| `vectors/entry-paths.json` | The golden path vectors: input → normalized path or rejection (see [Path vectors](#path-vectors)) |
| `check_schemas.py` | Optional cross-check with python-jsonschema |
| `../contract_fixtures.{h,cpp}` | The test harness: normalise, validate, compare or rewrite |
| `../json_schema_check.{h,cpp}` | The schema checker the C++ tests use |
| `../unit/contract_test.cpp` | The offline fixtures (unit binary) |
| `../unit/entry_path_test.cpp` | The path vectors, through `ddb::normalizeEntryPath` and `DDBNormalizePath` |
| `../integration/contract_drone_image_test.cpp` | The drone image fixtures (downloads one asset) |

## Inputs

The contract dataset is a copy of `inputs/` in a test area, every file and folder with the
modification time 1700000000 (2023-11-14T22:13:20Z), indexed with `DDBAdd`. Committed inputs
make hashes and sizes deterministic; the fixed mtime makes entry mtimes and the STAC datetimes
taken from them deterministic.

| File | Entry type | What it is |
| --- | --- | --- |
| `a.txt` | 2 Generic | 31 bytes of text |
| `cloud.las` | 5 PointCloud | 16 points, LAS 1.2 format 0, EPSG:32632 (PDAL `translate` from a CSV grid; creation date 2024-001) |
| `layer.geojson` | 14 Vector | A polygon and a point, EPSG:4326 |
| `ortho.tif` | 4 GeoRaster | 32x32 RGB Byte GeoTIFF, EPSG:32632, 0.5 m pixels (GDAL) |
| `sub/img.jpg` | 3 GeoImage | 32x24 JPEG with EXIF Make/Model, GPS position and altitude, GPS date and time stamp (Pillow) |

The capture time of `sub/img.jpg` comes from its GPS date/time stamp (UTC), so it does not
depend on a time zone. The drone image fixtures use `images/DJI_0018.JPG` of DroneDB/test_data
at the commit in `../test-data.sha` (its capture time uses the zoneinfo database that ships
with ddb).

## Fixtures

| Fixture | Call | Schema | Normalised |
| --- | --- | --- | --- |
| `add` | `DDBAdd(<root>, [<root>/a.txt, <root>/cloud.las, <root>/layer.geojson, <root>/ortho.tif, <root>/sub], recursive=true)` | `entries` | - |
| `list` | `DDBList(<root>, [<root>], "json", recursive=true)` | `entries` | - |
| `list-folder` | `DDBList(<root>, [<root>/sub], "json", recursive=false)` | `entries` | - |
| `list-entries` | `DDBListEntries(<root>, "", recursive=true, "", "", withDetails=true)` | `entries` | - |
| `list-entry-summaries` | `DDBListEntries(<root>, "", recursive=true, "", "", withDetails=false)` | `entry-summaries` | - |
| `list-entries-filtered` | `DDBListEntries(<root>, "", recursive=true, "*.TIF", "georaster,image", withDetails=false)` | `entry-summaries` | - |
| `get-image`, `get-folder` | `DDBGet(<root>, "sub/img.jpg")`, `DDBGet(<root>, "sub")` | `entry` | - |
| `info-generic`, `info-georaster`, `info-image`, `info-pointcloud`, `info-vector` | `DDBInfo([<root>/<file>], "json", withHash=true)` | `entries` | `depth` |
| `meta-add`, `meta-set`, `meta-get` | `DDBMetaAdd`, `DDBMetaSet`, `DDBMetaGet` of a singular key | `meta-item` | `id`, `mtime` |
| `meta-get-list` | `DDBMetaGet(<root>, "", "tags")` (a key ending in s holds a list) | `meta-items` | `id`, `mtime` |
| `meta-list`, `meta-list-entry` | `DDBMetaList(<root>, "")`, `DDBMetaList(<root>, "sub/img.jpg")` | `meta-list` | - |
| `meta-dump` | `DDBMetaDump(<root>, "[]")` | `meta-dump` | `id`, `mtime`; sorted by path, key, data |
| `meta-unset`, `meta-remove` | `DDBMetaUnset`, `DDBMetaRemove` | `meta-count` | - |
| `build-status` | `DDBBuild(<root>, "ortho.tif")`, then `DDBGetBuildStatus` of a built, a buildable, a non-buildable and a missing path | `build-status` | - |
| `pending-build-info-none` | `DDBGetPendingBuildInfo(<root>)`, nothing pending | `pending-build-info` | - |
| `pending-build-info` | A Shapefile without `.shx`/`.dbf`: `DDBBuild` returns `DDBERR_BUILDDEPMISSING`, then `DDBGetPendingBuildInfo` | `pending-build-info` | `lastAttempt`, `retryAfter` |
| `stac-collection` | `DDBStac(<root>, "", <collection URL>, "contract", <catalog URL>)` | `stac-collection` | - |
| `stac-item-georaster`, `stac-item-image` | `DDBStac(<root>, "ortho.tif" / "sub/img.jpg", ...)` | `stac-item` | - |
| `stac-item-collection` | `DDBStacItemCollection(<root>, ..., limit=10, offset=0)` | `stac-item-collection` | - |
| `stac-data-collection` | `DDBStacData(<root>, NULL)`: data only (see [Data-only STAC](#data-only-stac)) | `stac-data-collection` | - |
| `stac-data-item-georaster`, `stac-data-item-image` | `DDBStacData(<root>, "ortho.tif" / "sub/img.jpg")` | `stac-data-item` | - |
| `stac-data-item-collection` | `DDBStacItemCollectionData(<root>, "", "", limit=10, offset=0)` | `stac-data-item-collection` | - |
| `raster-info` | `DDBGetRasterInfo(<root>/ortho.tif)` | `raster-info` | - |
| `raster-metadata` | `DDBGetRasterMetadata(<root>/ortho.tif, NULL, NULL)`: per-band statistics | `raster-metadata` | - |
| `raster-metadata-formula` | `DDBGetRasterMetadata(<root>/ortho.tif, "VARI", "RGB")`: formula statistics and histogram | `raster-metadata` | - |
| `build-artifacts` | `DDBBuild(<root>, "ortho.tif")`, then `DDBGetBuildArtifacts` of a built (COG), two buildable and not built, a non-buildable file, a folder and a missing path (see [Build manifest](#build-manifest)) | `build-artifacts` | - |
| `build-artifacts-pending` | The `pending-build-info` dataset (`roads.shp` deferred), then `DDBGetBuildArtifacts(<root>, ["roads.shp"])` | `build-artifacts` | `lastAttempt`, `retryAfter` |
| `list-entries-ex` | `DDBBuild(<root>, "ortho.tif")`, then `DDBListEntriesEx(<root>, "", "", "", {recursive=true, withDetails=false, withBuild=true})` | `list-entries-ex` | - |
| `resolve-build-file` | `DDBResolveBuildFile(<root>, <hash of ortho.tif>, "cog/cog.tif")` after its build: an object with the absolute `path` and the artifact's `mediaType` | `build-file` | - |
| `copy-build-artifacts` | `DDBCopyBuildArtifacts(<root>, <root2>, <hash of ortho.tif>)` after its build, `<root2>` a dataset with `ortho.tif` | `copy-build-artifacts` | - |
| `entry-types` | `DDBGetEntryTypes()` (no dataset) | `entry-types` | - |
| `capabilities` | `DDBGetCapabilities()` (no dataset): ABI version, error codes, error reasons, cancellable exports, the symbol manifest | `capabilities` | `version` |
| `effective-config` | `DDBConfigure({"cacheDir": <cache>, "cpuPoolSize": 3, "ioThreads": 2, "minFreeDiskMb": 256})`, then `DDBGetEffectiveConfig()` (no dataset; see `ddb.h`, "Process configuration") | `effective-config` | `hardwareThreads` |
| `normalize-path`, `normalize-path-rejected` | `DDBNormalizePath("photos\\2024/./a.jpg", "[\".uploads\"]")`, `DDBNormalizePath(".uploads/x", "[\".uploads\"]")` (no dataset) | `normalize-path` | - |
| `info-drone-image`, `get-drone-image`, `stac-item-drone-image` | `DDBInfo`, `DDBGet`, `DDBStac` of `DJI_0018.JPG` (integration) | `entries`, `entry`, `stac-item` | `depth` (info) |

Each fixture file holds:

```json
{
  "description": "the input",
  "call": "the libddb call, with <root> for the dataset folder",
  "schema": "entries.schema.json",
  "normalisation": {"placeholders": ["<root>"], "volatileKeys": ["depth"]},
  "output": "the normalised output"
}
```

## Data-only STAC

`DDBStacData` and `DDBStacItemCollectionData` (2.0.0) return the documents of `DDBStac`
(with `id` NULL) and `DDBStacItemCollection` without any member built from a URL or an ID, which
Registry owns: no `id`, `collection` or `links`, and no asset `href`. Every other member is the
same, value for value (`ContractFixtures.StacDataIsStacWithoutUrlsAndIds` compares them). A
caller builds the rest from what is left:

* an Item's entry path is `properties.title` and the key of its data asset (role `data`); the
  `thumbnail` asset (role `thumbnail`) is there when libddb can render a thumbnail of the entry;
* a Collection's assets are keyed by entry path; its `title` is the dataset name, else the dataset
  folder name;
* `DDBStacData` returns `DDBERR_NOTFOUND` for an entry that is not in the index (`DDBStac`
  keeps returning `DDBERR_EXCEPTION`).

The existing `DDBStac` and `DDBStacItemCollection` outputs are unchanged (their fixtures did not
change).

## Build manifest

`DDBGetEntryTypes`, `DDBGetBuildArtifacts`, `DDBResolveBuildFile`, `DDBCopyBuildArtifacts` and
`DDBListEntriesEx` (2.0.0) let a host serve builds without knowing their layout.
The vocabulary and the fields are documented in `ddb.h` ("Build manifest");
`build-artifacts.schema.json` holds the shared shapes: `$defs/build` (the build object of an
entry, also embedded by `DDBListEntriesEx`) and `$defs/artifact`.

* An artifact is identified by its `role`; its `path` is relative to the build `root` and to the
  URL `.../build/{hash}/`. A host never composes `cog/cog.tif` itself.
* Only artifacts that exist are listed; a directory artifact (`kind: "dir"`) exists when its
  `index` does. A `legacy` artifact (`pointcloud.ept`) names the role it stands in for in
  `fallbackFor` (`pointcloud.copc`) and is listed only while that role is not.
* `mediaType` is the media type of the file a client opens: the file itself, or the index of a
  directory (so `vector.mvt` is `application/json`, its TileJSON; the tiles are served by file
  name). The values: `raster.cog` `image/tiff; application=geotiff; profile=cloud-optimized`,
  `pointcloud.copc` `application/vnd.laszip+copc`, `vector.gpkg`
  `application/geopackage+sqlite3` (the IANA registration), `model.bm3d`
  `application/vnd.dronedb.bm3d`, `model.nexus`, `gsplat.lod`,
  `gsplat.spz`, `gsplat.export` `application/octet-stream`, the JSON artifacts and directory indexes
  `application/json`.
* `downloadName` is set for the artifacts a download offers: `cog.tif`, `cloud.copc.laz`,
  `source.gpkg`, and for a Gaussian splat `model.rad` (`gsplat.lod`), `model.spz` (the legacy
  `gsplat.spz`) and `export.spz` (`gsplat.export`).
* `state` reads the build lock without reclaiming it (`BuildLock::isHeld`), so a listing never
  changes the dataset.
* `retryAfter` is the Unix second from which a pending build is retried by `DDBBuild` with
  `pendingOnly` (and no `force`): `lastAttempt` + libddb's retry backoff (60 s). It is null exactly
  when `lastAttempt` is, and is a time, not a "due now" flag: the retry is due when the current
  time is at or past it. `DDBGetPendingBuildInfo` reports the same value. A host compares it with
  its clock and keeps no copy of the backoff.

`DDBResolveBuildFile` returns a JSON object, `{"path": "<absolute path>", "mediaType": "<type>"}`:
`mediaType` is the one of the manifest when the file is a declared artifact (a file artifact, or
the index of a directory artifact) and is absent for any other file of a directory artifact (its
tiles). The path sits under the `<root>` placeholder in the fixture.

## Path vectors

`vectors/entry-paths.json` pins libddb's dataset path rules (`src/include/entry_path.h`,
`DDBNormalizePath` in `ddb.h`). libddb's unit tests run every vector, and Registry's
`DatasetPathPolicy` tests are meant to run the same file against `DDBNormalizePath`, so both
sides accept and reject the same paths.

```json
{
  "description": "...",
  "vectors": [
    {"input": "sub/.ddb/x", "expected": {"rejected": "reserved"}, "note": "optional, for humans"},
    {"input": ".uploads/x", "reserved": [".uploads"], "expected": {"rejected": "reserved"}},
    {"input": "a\u0000b", "cString": false, "expected": {"rejected": "nul"}}
  ]
}
```

| Key | Meaning |
| --- | --- |
| `input` | The path, a JSON string (UTF-8) |
| `reserved` | Optional: the `reservedJson` argument, an array of names reserved as the first segment only. Absent: `NULL` |
| `cString` | Optional, default `true`. `false` for an input with a NUL byte: the C API takes a NUL-terminated string and cannot receive it, so only `ddb::normalizeEntryPath` runs it (Registry rejects NUL before it calls libddb) |
| `expected` | The exact output of `DDBNormalizePath`: `{"path": "<normalized>"}` or `{"rejected": "<code>"}` (`schemas/normalize-path.schema.json`) |
| `note` | Optional explanation |

The rules, checked in this order: `empty` (the input is ""), `nul`, `encoding` (not valid UTF-8),
`absolute` (a leading `/` or `\`, which covers POSIX roots, UNC and device paths, or a drive
`X:`), `traversal` (a `..` segment), `empty` (nothing left once empty and `.` segments are
dropped), `reserved` (a `.ddb` segment at any depth, or a caller name as the first segment). `/`
and `\` both separate segments, as in Registry. Names are compared byte for byte (`.DDB`,
`.ddbx` and `photos/.uploads/x` are ordinary) and are not Unicode-normalized. A JSON string
cannot hold invalid UTF-8, so the `encoding` rejection is tested in `entry_path_test.cpp` only.

## Normalisation

Only values that change from run to run or from machine to machine are normalised, and the
normalised output keeps the JSON types, so it still matches its schema.

| Normalisation | Applies to | Why |
| --- | --- | --- |
| `<root>` placeholder: the absolute dataset folder, in its native and generic form, in every string; after it `\` becomes `/` | every fixture (in practice the `DDBInfo` paths) | the test area is a temporary folder |
| volatile key `depth`: value 0 | `info-*` | `DDBInfo` counts the folders of the absolute path |
| volatile keys `id` (`"<volatile>"`) and `mtime` (0) | meta items, meta dump | a new random UUID and the current time on every write |
| volatile keys `lastAttempt` and `retryAfter`: value 0 | `pending-build-info`, `build-artifacts-pending` | the time of the build attempt, and the retry time derived from it (the tests check `retryAfter` = `lastAttempt` + 60 before normalising) |
| volatile key `version`: `"<volatile>"` | `capabilities` | the library version and git commit of the build |
| `<cache>` placeholder, volatile key `hardwareThreads`: value 0 | `effective-config` | the cache folder is a temporary folder; the cores of the machine |
| sorted by `path`, `key`, `data` | `meta-dump` | libddb orders the dump by `id`, a random UUID |

Not normalised, fixed at the source instead: mtimes (set on the inputs), hashes and sizes
(committed inputs), capture times (GPS time stamp, shipped zoneinfo).

The comparison is strict on structure, keys and JSON types (an integer never equals a float, so
`1700000000` turning into `1700000000.0` fails). Floats are equal within a relative 1e-9, so the
last-digit differences between compilers and math libraries (Linux and Windows CI) do not fail.

## Validation

The C++ tests validate every output twice (raw and normalised) with `json_schema_check.h`, a
checker for the subset of draft 2020-12 these schemas use: no JSON Schema library is among the
vcpkg dependencies, and none was added. It rejects any keyword outside the subset, so a schema
cannot rely on a keyword that would be ignored. `ContractFiles.FixturesMatchTheirSchemas` checks
that every committed fixture matches its schema and that every schema has a fixture.

The reference implementation can cross-check the same files:

```sh
pip install jsonschema   # >= 4.18
python3 -I tests/contract/check_schemas.py
```

## Changing an output

A test fails with a JSON patch from the fixture to the new output. When the change is intended:

```sh
cd build
DDB_UPDATE_CONTRACT_FIXTURES=1 ./ddb_unit_tests --gtest_filter='Contract*'
DDB_UPDATE_CONTRACT_FIXTURES=1 DDB_TEST_CACHE_DIR=... ./ddbtest --gtest_filter='ContractFixturesDroneImage*'
git diff ../tests/contract
```

Review the diff, adjust the schema if a field or type changed, and list the change below. The
ABI rule applies (XI): outputs only grow; renaming or retyping a field is a breaking change that
Registry has to adopt first.

### Changes

* 2026-10-21: baseline (DroneDB `fixes-and-optimizations` after `7ee39d2`).
* 2026-10-21: `capabilities` added (2.0.0). Its `symbols` list is
  `src/include/ddb.symbols`, so every new export updates this fixture too.
* 2026-10-21: `normalize-path`, `normalize-path-rejected` and `vectors/entry-paths.json` added
  (2.0.0: `DDBNormalizePath`); `capabilities` lists the new export.
* 2026-10-21: `stac-data-collection`, `stac-data-item-georaster`, `stac-data-item-image` and
  `stac-data-item-collection` added with their schemas (2.0.0: `DDBStacData`,
  `DDBStacItemCollectionData`); `capabilities` lists the new exports. The shared parts of
  `stac-item.schema.json` (`stacExtensions`, `geometry`, `bbox`, `properties`) and
  `stac-collection.schema.json` (`extent`) moved to their `$defs`, unchanged, so the data-only
  schemas reference them.
* 2026-10-21: `capabilities` gains the error codes `INVALIDARGS` (7), `RETRYABLE` (8) and
  `SHUTDOWN` (9), the `errorReasons` array (the reasons of `DDBGetLastErrorReason`) and the new
  export `DDBGetLastErrorReason` (2.0.0).

* 2026-10-21: `build-artifacts`, `build-artifacts-pending`, `list-entries-ex`,
  `resolve-build-file`, `copy-build-artifacts` and `entry-types` added with their schemas
  (2.0.0: `DDBGetBuildArtifacts`, `DDBListEntriesEx`, `DDBResolveBuildFile`,
  `DDBCopyBuildArtifacts`, `DDBGetEntryTypes`; see [Build manifest](#build-manifest));
  `capabilities` lists the new exports. The existing fixtures did not change.

* 2026-10-21: `effective-config` added with its schema (2.0.0: `DDBConfigure`,
  `DDBGetEffectiveConfig`); `capabilities` lists the new exports. The existing fixtures did not
  change.

* 2026-10-21: `capabilities` lists the new exports for the process configuration, shutdown, cancellation and logging (2.0.0):
  `DDBShutdown`, `DDBCreateCancelToken`, `DDBCancel`, `DDBFreeCancelToken`,
  `DDBSetThreadCancelToken`, `DDBSetLogCallback`, `DDBSetThreadOperation`. Its `cancellable` list
  now names the exports whose work checks a cancel token (the render paths, the thumbnails,
  `DDBExportRaster`/`DDBExportRaster2`, `DDBBuild`) instead of `DDBExportRaster2` only; the
  schema describes it so. The other fixtures did not change.

* 2026-10-21: `capabilities` lists the new export `DDBApplyDeltaEx` (2.0.0). Its output
  is the conflict list of `DDBApplyDelta` (a JSON array of paths), so it has no fixture of its
  own. The other fixtures did not change.

* 2026-10-22: the build object (`build-artifacts`, `build-artifacts-pending`, `list-entries-ex`)
  and `pending-build-info` gain `retryAfter`: when the pending build is retried
  (`lastAttempt` + the 60 s backoff of `buildPending`), integer Unix seconds, null when
  `lastAttempt` is. Additive: no other field changed. In the two pending fixtures it is a volatile
  key next to `lastAttempt`. Registry's exact-JSON adapter tests of these outputs
  (`Abi11ContractTests`) need the field before Registry moves to a libddb build that has it.

* 2026-10-23: `entry-types` reports `thumbnail: "file"` for `Panorama` and `GeoPanorama` (it was
  `"none"` in 2.0.0, which made Registry stop rendering their thumbnails: they are JPEG/TIFF images
  rendered from the file like an `Image`). `Video` and `GeoVideo` stay `"none"`. A value of an
  existing field changes (the schema's enum already allows it): Registry follows the field, so a
  Registry on the new libddb renders panorama thumbnails again. No other fixture changed.

* 2026-10-24: every export that takes a dataset-relative entry path applies the rules of
  `DDBNormalizePath` to it (behaviour change, no output changed, so no fixture
  did): `\` and `/` separate segments, `.` segments and repeated or trailing separators are
  dropped, and a path that is absolute, has a `..` segment, names a `.ddb` folder or is not valid
  UTF-8 is refused with `DDBERR_INVALIDARGS` (also by the exports of 1.x, which keep
  `DDBERR_EXCEPTION` for their other invalid arguments), with the message `Invalid <argument>
  '<path>': <code>` and an empty `DDBGetLastErrorReason()`. A valid path behaves as before (the
  vectors above are the same input Registry's `DatasetPathPolicy` already sends). Covered:
  `DDBGet`, `DDBListEntries`, `DDBListEntriesEx` (a folder: `""` and `"."` are the root; a
  `..` segment is refused, where `a/../b` used to be resolved), `DDBSearch` (the query is a
  pattern over entry paths), `DDBMoveEntry` (a trailing separator is dropped, where it used to be
  an error), `DDBBuild` (`source`), `DDBIsBuildable`, `DDBIsBuildActive`, `DDBIsBuildComplete`,
  `DDBGetBuildStatus`, `DDBGetBuildArtifacts` (the `path` members of their outputs are the
  normalized spelling), `DDBMeta*` (`path`, and the rows of `DDBMetaRestore`), `DDBStac`,
  `DDBStacData`, and the paths of a delta (`DDBApplyDelta`, `DDBApplyDeltaEx`,
  `DDBComputeDeltaLocals`, `verifiedHashes` keys: a delta path in `.ddb`, or spelled with `\`,
  used to be accepted). Not covered, because they are filesystem paths: `DDBAdd*`, `DDBRemove`,
  `DDBList`, `DDBInfo`, dataset folders, output files, `DDBBuild`'s `dest`. The tests are
  `tests/unit/c_abi_entry_path_test.cpp` (every vector through every export).

* 2026-10-24: `effective-config` gains the setting `gdalCacheMb` and its source:
  an integer from 16 to 1048576 (MiB), the size of GDAL's block cache for the whole process. The
  fixture sets it to 256 through `DDBConfigure`; without it the value is the bound from
  `DDB_GDAL_CACHE_MB` or 1024, source `environment` or `default`. Additive: the other
  settings did not change.

* 2026-10-24: the `cancellable` list of `capabilities` grows (the analyses, the
  delta and the masks check the token too): `DDBGetRasterMetadata`, `DDBGetRasterValueInfo`, `DDBGetRasterPointValue`,
  `DDBGetRasterAreaStats`, `DDBGetRasterProfile`, `DDBCalculateVolume`, `DDBDetectStockpile`,
  `DDBDetectAllStockpiles`, `DDBGenerateContours`, `DDBMaskBorders`, `DDBComputeDeltaLocals`,
  `DDBApplyDelta` and `DDBApplyDeltaEx` (13 exports, 26 in all). Additive: no other member and no
  other fixture changed. `DDBApplyDelta(Ex)` is atomic to a cancel: it stops with
  `DDBERR_CANCELED` while it reads and hashes the added files, with the dataset and the source
  files untouched, and once it starts to change the dataset it applies the delta whole and
  returns `DDBERR_NONE` (a cancel or a shutdown that comes later is not seen). A canceled
  `DDBMaskBorders` leaves no output file.

* 2026-10-25: the artifact role `gsplat.georef` is removed from the manifest: the `roles` of the
  `GaussianSplat` entry type in `entry-types` are now `gsplat.lod`, `gsplat.spz` and
  `gsplat.bounds`, and `DDBGetBuildArtifacts` and `DDBListEntriesEx` no longer list a
  `gsplat/georef.json` artifact. No build ever wrote that file (libddb has no way to give a splat
  a georeference), so no output lost data; a host that reads the role list must not require it.
  The `properties` of a Gaussian splat entry no longer carry `"georeferenced": false` either; a
  splat is never georeferenced, so a host reads the absent member as false. The `entry-types`
  fixture changed; no other fixture did.

* 2026-10-27: an artifact may carry `fallbackFor` (additive; only legacy artifacts do): the role
  it stands in for. `pointcloud.ept` has `"fallbackFor": "pointcloud.copc"`. A legacy artifact
  is listed while that role is missing, as before for a point cloud; the rule no longer depends
  on whether other current artifacts of the type exist. No fixture changed (none lists a legacy
  artifact).

* 2026-10-28: the artifact `gsplat.spz` is legacy (`"fallbackFor": "gsplat.lod"`): a splat build
  made before the RAD layout lists it while `gsplat.lod` is missing, and a build that publishes
  `gsplat/model.rad` removes `gsplat/model.spz` alone (the folder is shared). The roles of
  `entry-types` and every fixture are unchanged: none of them lists the `legacy` or `fallbackFor`
  of an artifact for this type.

* 2026-10-28: `DDBResolveBuildFile` returns an object `{"path", "mediaType"?}` instead of a plain
  path string (still 2.0.0, ABI 2.0: the export is new in 2.0 and no release carries the string).
  `mediaType` is the manifest's type of the artifact when the file is a file artifact or the index
  of a directory artifact, and absent otherwise (tiles). A host that serves the file with the
  manifest's type reads it from here and falls back to its own mapping when it is absent; a host
  built for the string reads `path` when the output starts with `{`. `resolve-build-file` and its
  schema `build-file.schema.json` changed; no other fixture did.
  STAC names the `.spz`, `.splat` and `.ksplat` data asset `application/octet-stream`, the media
  type of the manifest (it said `application/x-spz` for `.spz`).

* 2026-10-28: the `properties` of an entry may carry `georef` (additive), with `georeferenced: true`
  and the origin of the local frame as the point geometry of the entry: `{"kind": "enu", "origin":
  [latitude, longitude, altitude]}` or `{"kind": "projected", "crs": "<EPSG:xxxx, PROJ string or
  WKT>", "offset": [x, y, z]}` (a point of the file plus `offset` is in `crs`). A Gaussian splat
  with a georeferencing sidecar (`<stem>.geo.json`, `georef.json`, `reference_lla.json` or the
  ODM `coords.txt`, see `detectLocalFrameGeoref`) now has them, as a georeferenced model has the
  first two (a model gains `georef`); a splat without one still carries neither, as noted on
  2026-10-25. The `bounds.json` of the built splat gains `up: [0, 0, 1]`, `upConfidence: 1` and
  `upSource: "georef"` and `wgs84Footprint`, a GeoJSON Polygon geometry (`[longitude, latitude]`
  ring) of its bounding box. `entry.schema.json` describes the two members; no fixture changed.

* 2026-10-28: the export `DDBExportSplatTrainingData(ddbPath, folder, outFolder, optionsJson,
  progress, progressUserData, output)` is new in 2.0.0 (ABI 2.0, no bump). It writes the training
  data of a Gaussian splat (a COLMAP sparse model, the images and `georef.json`) from the ODM
  project that is in `folder` of the dataset. Its output is `{"warnings": [string], "images": n,
  "points": n}`; the options are `{"maxImageSize"?: n, "maxPoints"?: n}` (whole numbers from 0, 0 for
  the originals and for no points). It returns `DDBERR_INVALIDARGS` for an option that is not usable,
  `DDBERR_NOTFOUND` for a folder that is not in the dataset, `DDBERR_EXCEPTION` for a file of the
  project that is missing or unusable (the message names `coords.txt`, `cameras.json`... ) and
  `DDBERR_CANCELED` when
  the token is canceled or the progress callback returns non-zero. `capabilities` lists the export in
  `symbols` and in `cancellable`; no other fixture changed.

* 2026-10-28: a Gaussian splat has a new artifact role, `gsplat.export` (`gsplat/export.spz`,
  `application/octet-stream`, `downloadName` `export.spz`): the splat as one `.spz` file for other
  tools, made on demand and never by a build. It is listed (last) only while the file exists, is not
  `legacy` and has no `fallbackFor`, is not part of the completeness of a build (`complete` still
  means `gsplat/model.rad`), and goes with `gsplat/` when the entry is built again. The artifacts
  `gsplat.lod` and `gsplat.spz` gain a `downloadName` (`model.rad`, `model.spz`), so a host that
  offers the first artifact with a `downloadName` as the download of an entry (Registry's
  `build-artifact`) serves a splat. The `entry-types` fixture lists the new role; no other fixture
  changed (none lists a splat build).
