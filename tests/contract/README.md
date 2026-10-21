# libddb contract fixtures (XI-09)

The JSON outputs of libddb that Registry consumes, recorded as golden fixtures with a JSON
Schema each. They freeze today's field names, types and values: any unplanned change to one of
these outputs fails a DroneDB test, and Registry's contract tests (RQ-04, `FakeDdbWrapper`) read
the same files.

The fixtures record the outputs **as they are today** (libddb 1.12.2, ABI 1.0). Nothing was
renamed or retyped for them, including the mixed casing:

* `point_geom` and `polygon_geom` are snake_case; the entry `properties` keys (`captureTime`,
  `bandCount`, ...) are camelCase; STAC keeps its own `stac_version` and `stac_extensions`.
* `mtime` (entries and meta items) and `lastAttempt` are integer Unix epoch **seconds**.
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
| `pending-build-info` | A Shapefile without `.shx`/`.dbf`: `DDBBuild` returns `DDBERR_BUILDDEPMISSING`, then `DDBGetPendingBuildInfo` | `pending-build-info` | `lastAttempt` |
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
| `build-artifacts-pending` | The `pending-build-info` dataset (`roads.shp` deferred), then `DDBGetBuildArtifacts(<root>, ["roads.shp"])` | `build-artifacts` | `lastAttempt` |
| `list-entries-ex` | `DDBBuild(<root>, "ortho.tif")`, then `DDBListEntriesEx(<root>, "", "", "", {recursive=true, withDetails=false, withBuild=true})` | `list-entries-ex` | - |
| `resolve-build-file` | `DDBResolveBuildFile(<root>, <hash of ortho.tif>, "cog/cog.tif")` after its build: a path, held as a JSON string | `build-file` | - |
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

`DDBStacData` and `DDBStacItemCollectionData` (XI-09, ABI 1.1) return the documents of `DDBStac`
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
`DDBListEntriesEx` (XI-04, ABI 1.1) let a host serve builds without knowing their layout
(decision D8). The vocabulary and the fields are documented in `ddb.h` ("Build manifest");
`build-artifacts.schema.json` holds the shared shapes: `$defs/build` (the build object of an
entry, also embedded by `DDBListEntriesEx`) and `$defs/artifact`.

* An artifact is identified by its `role`; its `path` is relative to the build `root` and to the
  URL `.../build/{hash}/`. A host never composes `cog/cog.tif` itself.
* Only artifacts that exist are listed; a directory artifact (`kind: "dir"`) exists when its
  `index` does. A `legacy` artifact (`pointcloud.ept`) is listed only when none of the type's
  current artifacts exists.
* `mediaType` is the media type of the file a client opens: the file itself, or the index of a
  directory (so `vector.mvt` is `application/json`, its TileJSON; the tiles are served by file
  name). The values: `raster.cog` `image/tiff; application=geotiff; profile=cloud-optimized`,
  `pointcloud.copc` `application/vnd.laszip+copc`, `vector.gpkg`
  `application/geopackage+sqlite3` (the IANA registration), `model.nexus`, `gsplat.lod`,
  `gsplat.spz` `application/octet-stream`, the JSON artifacts and directory indexes
  `application/json`.
* `downloadName` is set for the three artifacts a download offers today: `cog.tif`,
  `cloud.copc.laz`, `source.gpkg`.
* `state` reads the build lock without reclaiming it (`BuildLock::isHeld`), so a listing never
  changes the dataset.

`DDBResolveBuildFile` returns a plain string, not JSON: its fixture holds it as a JSON string
under the `<root>` placeholder.

## Path vectors

`vectors/entry-paths.json` pins libddb's dataset path rules (XI-09, `src/include/entry_path.h`,
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
| volatile key `lastAttempt`: value 0 | `pending-build-info` | the time of the build attempt |
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
* 2026-10-21: `capabilities` added (XI-01, ABI 1.1). Its `symbols` list is
  `src/include/ddb.symbols`, so every new export updates this fixture too.
* 2026-10-21: `normalize-path`, `normalize-path-rejected` and `vectors/entry-paths.json` added
  (XI-09, ABI 1.1: `DDBNormalizePath`); `capabilities` lists the new export.
* 2026-10-21: `stac-data-collection`, `stac-data-item-georaster`, `stac-data-item-image` and
  `stac-data-item-collection` added with their schemas (XI-09, ABI 1.1: `DDBStacData`,
  `DDBStacItemCollectionData`); `capabilities` lists the new exports. The shared parts of
  `stac-item.schema.json` (`stacExtensions`, `geometry`, `bbox`, `properties`) and
  `stac-collection.schema.json` (`extent`) moved to their `$defs`, unchanged, so the data-only
  schemas reference them.
* 2026-10-21: `capabilities` gains the error codes `INVALIDARGS` (7), `RETRYABLE` (8) and
  `SHUTDOWN` (9), the `errorReasons` array (the reasons of `DDBGetLastErrorReason`) and the new
  export `DDBGetLastErrorReason` (XI-02, DQ-10, ABI 1.1).

* 2026-10-21: `build-artifacts`, `build-artifacts-pending`, `list-entries-ex`,
  `resolve-build-file`, `copy-build-artifacts` and `entry-types` added with their schemas
  (XI-04, ABI 1.1: `DDBGetBuildArtifacts`, `DDBListEntriesEx`, `DDBResolveBuildFile`,
  `DDBCopyBuildArtifacts`, `DDBGetEntryTypes`; see [Build manifest](#build-manifest));
  `capabilities` lists the new exports. The existing fixtures did not change.

* 2026-10-21: `effective-config` added with its schema (DQ-15, XI-06, ABI 1.1: `DDBConfigure`,
  `DDBGetEffectiveConfig`); `capabilities` lists the new exports. The existing fixtures did not
  change.

## Cross-repository CI

The `registry_contract` job of `.github/workflows/c-cpp.yml` installs the `.deb` that
`build_ubuntu` tested, checks out DroneDB/Registry at the commit in `ci/registry-ref` and runs
`Registry.Adapters.Ddb.Test`, which fails on a missing export. On `v*` tags the release upload
of the `.deb` (`release_assets`) and the Docker image wait for it.
