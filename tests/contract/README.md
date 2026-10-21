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
| `check_schemas.py` | Optional cross-check with python-jsonschema |
| `../contract_fixtures.{h,cpp}` | The test harness: normalise, validate, compare or rewrite |
| `../json_schema_check.{h,cpp}` | The schema checker the C++ tests use |
| `../unit/contract_test.cpp` | The offline fixtures (unit binary) |
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
| `raster-info` | `DDBGetRasterInfo(<root>/ortho.tif)` | `raster-info` | - |
| `raster-metadata` | `DDBGetRasterMetadata(<root>/ortho.tif, NULL, NULL)`: per-band statistics | `raster-metadata` | - |
| `raster-metadata-formula` | `DDBGetRasterMetadata(<root>/ortho.tif, "VARI", "RGB")`: formula statistics and histogram | `raster-metadata` | - |
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

## Normalisation

Only values that change from run to run or from machine to machine are normalised, and the
normalised output keeps the JSON types, so it still matches its schema.

| Normalisation | Applies to | Why |
| --- | --- | --- |
| `<root>` placeholder: the absolute dataset folder, in its native and generic form, in every string; after it `\` becomes `/` | every fixture (in practice the `DDBInfo` paths) | the test area is a temporary folder |
| volatile key `depth`: value 0 | `info-*` | `DDBInfo` counts the folders of the absolute path |
| volatile keys `id` (`"<volatile>"`) and `mtime` (0) | meta items, meta dump | a new random UUID and the current time on every write |
| volatile key `lastAttempt`: value 0 | `pending-build-info` | the time of the build attempt |
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

## Cross-repository CI

The `registry_contract` job of `.github/workflows/c-cpp.yml` installs the `.deb` that
`build_ubuntu` tested, checks out DroneDB/Registry at the commit in `ci/registry-ref` and runs
`Registry.Adapters.Ddb.Test`, which fails on a missing export. On `v*` tags the release upload
of the `.deb` (`release_deb`) and the Docker image wait for it.
