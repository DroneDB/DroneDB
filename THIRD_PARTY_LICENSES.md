# Third-Party Licenses

DroneDB itself is distributed under the **Mozilla Public License 2.0**
(see [LICENSE.md](LICENSE.md)). This document lists the third-party
software that may be redistributed alongside DroneDB binaries (Windows
ZIP archive, Debian package, Docker image) and the licenses that apply
to those components.

DroneDB links against several open-source libraries provided through
[vcpkg](https://github.com/microsoft/vcpkg) at build time (GDAL, PROJ,
PDAL, SpatiaLite, libcurl, ...). On Linux they are linked **statically**
(the `x64-linux-release` triplet in [`vcpkg-triplets`](vcpkg-triplets) sets
static library linkage) into `libddb`, `ddbcmd` and the helper executables
below; the vcpkg libraries that ship as shared libraries in the Debian
package are PDAL (`libpdalcpp` and its plugins). On Windows
they are DLLs in the ZIP archive. The licenses of those libraries apply
to the redistributed binaries, and both packages ship the notice of
every one of them (see
[vcpkg-managed libraries](#vcpkg-managed-libraries) below). The sections
below focus on components that ship as separate binaries inside our
distribution archives.

Two GPL-licensed code bases are **not** part of `libddb` or `ddbcmd`:
Exiv2 (image metadata) lives in the separate `metadata-util` executable, and
Nexus with vcglib and corto (3D mesh streaming) in the separate
`ddb-nexus` executable. DroneDB runs both as child processes. The Linux
and Windows CI builds check this on the files they produce, each as far
as its platform's tools allow:

- On Linux, [`scripts/check-gpl-linkage.py`](scripts/check-gpl-linkage.py)
  reads every symbol defined in `libddb.so` and `ddbcmd` (none may come
  from Exiv2, Nexus, vcglib or corto) and the shared libraries they load
  (neither may load `libexiv2` or `libnxs`).
- On Windows, [`scripts/check-gpl-linkage.ps1`](scripts/check-gpl-linkage.ps1)
  reads only the DLLs `ddb.dll` and `ddbcmd.exe` import
  (`dumpbin /dependents`): neither may load `exiv2.dll` or `nxs.dll`. It
  does not look at symbols, so code compiled into them is not seen there.

Both also check that `ddb-nexus` and its Nexus library (`libnxs.so`,
`nxs.dll`) do not use Qt: on Linux by symbols and loaded libraries, on
Windows by the DLLs they import.

---

## Untwine (optional COPC accelerator)

When present in the distribution archive, the `untwine` /
`untwine.exe` binary is the upstream
[hobu/untwine](https://github.com/hobuinc/untwine) tool, vendored as a
git submodule under `vendor/untwine` and built unmodified against the
same vcpkg toolchain as DroneDB.

- **License**: GNU General Public License, version 3 (GPL-3.0-only).
- **Source**: https://github.com/hobuinc/untwine
- **Vendored copy**: [`vendor/untwine`](vendor/untwine) (git submodule);
  the upstream `LICENSE` file is preserved verbatim inside the
  submodule and is also distributed with the source release tarball.

### Why GPL is compatible with MPL-licensed DroneDB

DroneDB invokes `untwine` exclusively as a **separate subprocess**
through [`src/library/untwine_runner.cpp`](src/library/untwine_runner.cpp);
it is never linked (statically or dynamically) into `ddbcmd`,
`libddb`, or any other DroneDB-licensed binary. Per the
[GPL FAQ on aggregation](https://www.gnu.org/licenses/gpl-faq.html#MereAggregation),
this constitutes "mere aggregation" of independent programs and the
GPL terms therefore apply only to the `untwine` binary itself, not to
the rest of DroneDB.

### Obtaining the corresponding source

The `untwine` source code is publicly available at the upstream
repository linked above. The exact commit shipped with each DroneDB
release is recorded in the `vendor/untwine` submodule reference of the
corresponding git tag in this repository.

### Optionality

`untwine` is an **optional accelerator**: when the binary is missing
from the installation, DroneDB transparently falls back to PDAL's
`writers.copc` pipeline. Distributors who prefer not to redistribute
GPL code may safely omit the `untwine` / `untwine.exe` file from the
archive without affecting DroneDB's functionality (only the fast
disk-backed COPC build path is disabled).

---

## Obj2Tiles (optional OGC 3D Tiles generator)

When present in the distribution archive, the `Obj2Tiles` /
`Obj2Tiles.exe` binary is the upstream
[OpenDroneMap/Obj2Tiles](https://github.com/OpenDroneMap/Obj2Tiles)
tool, a precompiled .NET application redistributed **unmodified**
(the same precompiled release binaries used by OpenDroneMap).

- **License**: GNU Affero General Public License, version 3 (AGPL-3.0).
- **Source**: https://github.com/OpenDroneMap/Obj2Tiles
- **Distributed copy**: the precompiled `Obj2Tiles` / `Obj2Tiles.exe`
  binary placed next to the DroneDB executable (mirroring the
  OpenDroneMap packaging); the upstream `LICENSE.md` is preserved
  alongside it.

### Why AGPL is compatible with MPL-licensed DroneDB

DroneDB invokes `Obj2Tiles` exclusively as a **separate subprocess**
through [`src/library/obj2tiles_runner.cpp`](src/library/obj2tiles_runner.cpp);
it is never linked (statically or dynamically) into `ddbcmd`,
`libddb`, or any other DroneDB-licensed binary. Per the
[GPL FAQ on aggregation](https://www.gnu.org/licenses/gpl-faq.html#MereAggregation),
this constitutes "mere aggregation" of independent programs and the
AGPL terms therefore apply only to the `Obj2Tiles` binary itself, not
to the rest of DroneDB. DroneDB uses `Obj2Tiles` as an **offline build
tool** (it is not exposed as, or run as part of, a network service by
DroneDB), so AGPL's network-use clause is satisfied by providing a
reference to the upstream source below.

### Obtaining the corresponding source

The `Obj2Tiles` source code is publicly available at the upstream
repository linked above. The exact release version shipped with each
DroneDB release is recorded in the packaging scripts of the
corresponding git tag in this repository.

### Optionality

`Obj2Tiles` is an **optional generator**: when the binary is missing
from the installation, DroneDB skips OGC 3D Tiles generation for
`MODEL` entries and still produces the Nexus (`.nxz`) output, so no
build fails. Distributors who prefer not to redistribute AGPL code may
safely omit the `Obj2Tiles` / `Obj2Tiles.exe` file from the archive
without affecting DroneDB's core functionality (only the additive 3D
Tiles output is disabled).

---

## libktx / KTX-Software (bundled with Obj2Tiles for KTX2 texture compression)

When Obj2Tiles is asked to emit KTX2 textures (`--texture-format Ktx2`,
used by DroneDB's 3D Tiles build since Obj2Tiles v1.6.0), it loads the
[KTX-Software](https://github.com/KhronosGroup/KTX-Software) native
library (`ktx.dll` / `libktx.so` / `libktx.dylib`) via P/Invoke. This
library is bundled unmodified in the upstream Obj2Tiles release archive
and is copied next to the `Obj2Tiles` / `Obj2Tiles.exe` binary by
[`scripts/download-obj2tiles.ps1`](scripts/download-obj2tiles.ps1) /
[`scripts/download-obj2tiles.sh`](scripts/download-obj2tiles.sh).

- **License**: Apache License, version 2.0.
- **Source**: https://github.com/KhronosGroup/KTX-Software
- **Distributed copy**: the precompiled `ktx.dll` / `libktx.so` /
  `libktx.dylib` binary placed next to the Obj2Tiles executable.

Like Obj2Tiles itself, libktx is invoked only by the separate Obj2Tiles
subprocess and is never linked into `ddbcmd` / `libddb`; it does not
affect DroneDB's MPL-2.0 licensing. It is optional: when absent,
Obj2Tiles fails to honor `--texture-format Ktx2`, which DroneDB treats
as a best-effort 3D Tiles generation failure (same as a missing
Obj2Tiles binary), while Nexus output is unaffected.

---

## LibreDWG `dwg2dxf` (optional DWG converter)

When present in the distribution archive, the `dwg2dxf` /
`dwg2dxf.exe` binary is the
[GNU LibreDWG](https://www.gnu.org/software/libredwg/) DWG-to-DXF
converter, built **unmodified** from the signed GNU release tarball by
[`scripts/build-libredwg.sh`](scripts/build-libredwg.sh) (version in
[`scripts/libredwg-version`](scripts/libredwg-version); the tarball
signature is verified against the LibreDWG maintainer's key
`38A4167B0DB69E49C5F7216CB8C28866AB27A7A2`). Configure flags:
`--disable-shared --enable-static --disable-bindings --disable-docs
--disable-python --enable-release`; the Windows binary is
cross-compiled with mingw-w64 from the same tarball
(`LDFLAGS=-all-static`).

- **License**: GNU General Public License, version 3 or later
  (GPL-3.0-or-later).
- **Source**: https://www.gnu.org/software/libredwg/
- **Distributed copy**: the `dwg2dxf` / `dwg2dxf.exe` binary placed
  next to the DroneDB executable, with the upstream `COPYING` file
  next to it as `dwg2dxf.LICENSE.txt`.

### Why GPL is compatible with MPL-licensed DroneDB

DroneDB invokes `dwg2dxf` exclusively as a **separate subprocess**
through [`src/library/cad.cpp`](src/library/cad.cpp) (bounded by an
idle timeout and a total run time); LibreDWG is never linked
(statically or dynamically) into `ddbcmd`, `libddb`, or any other
DroneDB-licensed binary. Per the
[GPL FAQ on aggregation](https://www.gnu.org/licenses/gpl-faq.html#MereAggregation),
this constitutes "mere aggregation" of independent programs and the
GPL terms therefore apply only to the `dwg2dxf` binary itself.

### Obtaining the corresponding source

Every DroneDB release that ships `dwg2dxf` attaches the exact
`libredwg-<version>.tar.xz` it was built from, with its GNU signature
(`.sig`), to the same GitHub release. The build script and flags are in
the corresponding git tag of this repository. The same tarball is also
available from the GNU FTP server linked above.

### Optionality

`dwg2dxf` is an **optional converter**: when the binary is missing,
DWG files are still indexed (without a footprint) and their builds fail
with a message that names the missing tool; every other format,
including DXF, is unaffected. Distributors who prefer not to
redistribute GPL code may safely omit `dwg2dxf` / `dwg2dxf.exe` from
the archive.

---

## Exiv2 in `metadata-util` (image metadata helper)

DroneDB reads and writes EXIF, XMP and the other image metadata through
[Exiv2](https://exiv2.org/), which is linked into the separate
`metadata-util` / `metadata-util.exe` executable and nowhere else. The executable
is DroneDB's own program, kept in its own repository
([DroneDB/metadata-util](https://github.com/DroneDB/metadata-util), git submodule
[`vendor/metadata-util`](vendor/metadata-util)); it is required, and is installed
next to `ddb` (`/usr/bin` in the Debian package, the root folder of the
Windows ZIP archive).

- **License of Exiv2**: GNU General Public License, version 2 or later
  (GPL-2.0-or-later). The vcpkg port builds the upstream release archive
  with a few port patches.
- **License of `metadata-util`**: GNU General Public License, version 2 or
  later (GPL-2.0-or-later), the text is in
  [`vendor/metadata-util/LICENSE`](vendor/metadata-util/LICENSE). The protocol
  document `vendor/metadata-util/PROTOCOL.md` is released under CC0-1.0, so
  anyone can write a client or a replacement without taking over any GPL
  code.
- **Source**: https://exiv2.org/ and https://github.com/DroneDB/metadata-util
- **Distributed copy**: the `metadata-util` / `metadata-util.exe` binary (Exiv2
  and its vcpkg dependencies linked statically on Linux, `exiv2.dll`
  and the other DLLs next to it on Windows), with the licence text of
  `metadata-util` as `metadata-util.LICENSE` in `/usr/share/doc/ddb`
  (Debian package) or the root of the ZIP archive, and that of Exiv2 as
  `exiv2.LICENSE` in the `third-party` folder there, with the notices of
  the other vcpkg libraries.

### Why GPL is compatible with MPL-licensed DroneDB

`libddb` starts `metadata-util` exclusively as a **separate child process**
and talks to it over its standard input and output with the documented
JSON-lines protocol ([`vendor/metadata-util/PROTOCOL.md`](vendor/metadata-util/PROTOCOL.md):
a request line per file, a response line with the tags), which is the
whole boundary between the two programs. The client side is DroneDB's
own MPL-licensed code:
[`src/library/exif_helper_pool.cpp`](src/library/exif_helper_pool.cpp)
starts and reuses the `metadata-util` processes,
[`src/library/exif_protocol.cpp`](src/library/exif_protocol.cpp) writes
the requests and decodes the responses, and
[`src/include/exif_metadata.h`](src/include/exif_metadata.h) with
[`src/library/exif_metadata.cpp`](src/library/exif_metadata.cpp) holds
the tag model and the reader interface the rest of `libddb` goes
through. Exiv2 is never linked
(statically or dynamically) into `ddbcmd`, `libddb`, or any other
DroneDB-licensed binary, which the CI checks listed above enforce. Per
the [GPL FAQ on aggregation](https://www.gnu.org/licenses/gpl-faq.html#MereAggregation),
pipes and command lines are how separate programs normally communicate,
and the exchange is plain data, not shared internal structures; the GPL
terms therefore apply to `metadata-util` itself, not to the rest of DroneDB.

### Obtaining the corresponding source

The `metadata-util` source code is the `vendor/metadata-util` submodule of the
corresponding git tag of this repository, and is publicly available in
the `DroneDB/metadata-util` repository. Every DroneDB release attaches the
exact Exiv2 source archive that vcpkg built into `metadata-util`
(`exiv2-<version>.tar.gz`), the vcpkg port files with the patches vcpkg
applies to it (`vcpkg-ports-for-gpl-sources.tar.gz`) and a `SOURCES.txt`
that names the origin and SHA512 of each archive, to the same GitHub
release. [`scripts/fetch-vcpkg-gpl-sources.sh`](scripts/fetch-vcpkg-gpl-sources.sh)
collects them from the vcpkg port files of the tag.

### Optionality

`metadata-util` is **required**: without it DroneDB cannot read image
metadata, so indexing and building stop with an error that names the
missing tool and the `DDB_METADATA_UTIL_PATH` environment variable (which
overrides where DroneDB looks for it). Distributors who cannot
redistribute GPL code can provide their own executable that speaks the
CC0-licensed protocol and point `DDB_METADATA_UTIL_PATH` at it.

---

## `ddb-nexus` (Nexus 3D mesh streaming helper)

DroneDB builds the progressive Nexus format (`.nxz`) for 3D models with
the separate `ddb-nexus` / `ddb-nexus.exe` executable, built from
[DroneDB/libnexus](https://github.com/DroneDB/libnexus) (git submodule
[`vendor/libnexus`](vendor/libnexus)), a fork of
[cnr-isti-vclab/nexus](https://github.com/cnr-isti-vclab/nexus). It
loads the `libnxs.so` / `nxs.dll` library of the same fork, which ships
for it alone: neither `libddb` nor `ddbcmd` links or loads it.

- **License of `ddb-nexus` and the libnexus wrapper**: GNU General
  Public License, version 3 (GPL-3.0). The wrapper is DroneDB's own code
  (Digipa), relicensed from AGPL-3.0 so that it matches the code it is
  combined with; the text is in [`vendor/libnexus/LICENSE`](vendor/libnexus/LICENSE).
- **Nexus**: GNU General Public License, Visual Computing Lab,
  ISTI - CNR (`vendor/libnexus/vendor/nexus/src/LICENSE` is version 3; the
  source headers say version 2 or later).
- **vcglib**: GNU General Public License, Visual Computing Lab,
  ISTI - CNR (`vendor/libnexus/vendor/vcglib/LICENSE.txt` is version 3; the
  source headers say version 2 or later).
- **corto** (mesh compression): Visual Computing Lab, ISTI - CNR
  (`vendor/libnexus/vendor/nexus/src/corto/LICENSE.txt` is the MIT
  license; the headers of the library sources built into `ddb-nexus` say
  GNU Lesser General Public License, version 3 or later, and its README
  says the C++ code is under the GPL). Either reading is compatible with
  the GPL-3.0 of the executable.
- **Image codec libraries** (texture decoding and encoding), from the
  vcpkg ports of the same names, with permissive licences:
  [libjpeg-turbo](https://libjpeg-turbo.org/) (IJG License and
  3-clause BSD License), [libpng](http://www.libpng.org/pub/png/libpng.html)
  (PNG Reference Library License version 2),
  [libtiff](https://libtiff.gitlab.io/libtiff/) (libtiff licence, an
  MIT-style licence) and [libwebp](https://github.com/webmproject/libwebp)
  (3-clause BSD License), together with
  [stb](https://github.com/nothings/stb) (MIT or public domain, at your
  choice; no notice is required). `ddb-nexus` uses no GUI
  toolkit: it needs no Qt, X11, OpenGL or D-Bus library. On Linux the
  codecs are linked statically into `libnxs.so`, which ships only for
  `ddb-nexus`; on Windows their DLLs ship next to `ddb-nexus.exe`. The
  CI checks listed above also fail a build in which `ddb-nexus` or
  `libnxs` contains or loads Qt again.
- **Distributed copy**: the `ddb-nexus` / `ddb-nexus.exe` binary, with
  `ddb-nexus.LICENSE`, `Nexus.LICENSE`, `vcglib.LICENSE` and
  `corto.LICENSE` in `/usr/share/doc/ddb` (Debian package) or the root of
  the ZIP archive, and `libjpeg-turbo.LICENSE`, `libpng.LICENSE`,
  `tiff.LICENSE`, `libwebp.LICENSE` and `stb.LICENSE` in the
  `third-party` folder there. All of the GPL code is under GPL version 3
  or compatible terms; the image codec libraries are permissive and their
  notices ship with the binary.

### Why GPL is compatible with MPL-licensed DroneDB

`libddb` starts `ddb-nexus` exclusively as a **separate subprocess**
through [`src/library/nexus_runner.cpp`](src/library/nexus_runner.cpp) (an input model, an
output path, the exit status and a message on failure); Nexus, vcglib
and corto are never linked (statically or dynamically) into
`ddbcmd`, `libddb`, or any other DroneDB-licensed binary, which the CI
checks listed above enforce. Per the
[GPL FAQ on aggregation](https://www.gnu.org/licenses/gpl-faq.html#MereAggregation)
this constitutes "mere aggregation" of independent programs, and the GPL
terms therefore apply only to the `ddb-nexus` executable.

### Obtaining the corresponding source

The `ddb-nexus` source code (the wrapper, Nexus, vcglib and corto) is
the `vendor/libnexus` submodule of the corresponding git tag of this
repository, publicly available at the repository linked above. The image codec
libraries are permissive and need no source offer.

### Optionality

`ddb-nexus` is needed only for Nexus output: when it is missing, a build
that produces Nexus fails with a message that names the tool and the
`DDB_NEXUS_PATH` environment variable (which overrides where DroneDB
looks for it), and every other build type is unaffected. Distributors who prefer not to redistribute this code may
omit `ddb-nexus` and `libnxs.so` / `nxs.dll`.

---

## vcpkg-managed libraries

The libraries vcpkg builds (GDAL, PROJ, GEOS, PDAL, SQLite, libcurl,
OpenSSL, Exiv2, expat, zlib, brotli and the others of
[`vcpkg.json`](vcpkg.json) with their dependencies) are linked
statically into `libddb`, `ddbcmd` and the helper executables on Linux,
except PDAL (`libpdalcpp` and its plugins), and ship as DLLs in the
Windows ZIP. Each is distributed under its own license (typically MIT,
BSD, Apache 2.0, zlib, LGPL; Exiv2 is GPL, see above).

Both packages ship the notice of every vcpkg library that ends up in
them: `/usr/share/doc/ddb/third-party` in the Debian package and the
`third-party` folder of the Windows ZIP hold one `<port>.LICENSE` per
vcpkg port (the port's `share/<port>/copyright` file) and `index.txt`,
which lists the ports with their versions and the licenses vcpkg
records for them. [`scripts/collect-vcpkg-notices.py`](scripts/collect-vcpkg-notices.py)
fills the folder from the build's vcpkg tree, leaving out only the
ports that are never linked into a shipped binary (vcpkg's build
helpers, `pkgconf`, `pdal-dimbuilder` and the `gtest` test framework),
and [`scripts/check-helper-tools.py`](scripts/check-helper-tools.py)
fails the package build when the folder is missing, empty or lacks a
notice it lists.

GDAL (MIT) is built from the overlay port in
[`vcpkg-overlays/gdal`](vcpkg-overlays/gdal): the upstream vcpkg port
plus one source patch, `dxf-fit-point-splines.patch`, which makes the
DXF driver read SPLINE entities defined only by fit points (as written
by LibreDWG `dwg2dxf`) instead of dropping them.

---

## Reporting issues

If you spot a missing attribution or believe a license is being
infringed in our distribution archives, please open an issue at
https://github.com/DroneDB/DroneDB/issues so we can address it
promptly.
