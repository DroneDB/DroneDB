# Third-Party Licenses

DroneDB itself is distributed under the **Mozilla Public License 2.0**
(see [LICENSE.md](LICENSE.md)). This document lists the third-party
software that may be redistributed alongside DroneDB binaries (Windows
ZIP archive, Debian package, Docker image) and the licenses that apply
to those components.

DroneDB **dynamically links** against several open-source libraries
provided through [vcpkg](https://github.com/microsoft/vcpkg) at build
time (GDAL, PROJ, PDAL, SpatiaLite, Exiv2, libcurl, ...). The licenses
of those libraries apply to the redistributed binaries; refer to the
upstream projects for the exact terms. The list below is not
exhaustive - it focuses on components that ship as separate binaries
inside our distribution archives.

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

## libnexus / Nexus

The `libnxs.so` / `nxs.dll` library shipped with DroneDB is a fork of
[cnr-isti-vclab/nexus](https://github.com/cnr-isti-vclab/nexus)
(maintained at [DroneDB/libnexus](https://github.com/DroneDB/libnexus)),
distributed under the **GNU General Public License v3.0** (or later).
See `vendor/libnexus/LICENSE.txt` for the full text.

---

## vcpkg-managed runtime libraries

The Windows ZIP and the Debian package include several shared
libraries pulled in by vcpkg (notably `pdalcpp`, `gdal`, `proj`,
`libsqlite3`, `libcurl`, `dbus`, ...). Each of those libraries is
distributed under its own license (typically MIT, BSD, Apache 2.0,
LGPL, or X11). The relevant license files can be found inside
`build/vcpkg_installed/<triplet>/share/<port>/copyright` after a
build, and the upstream license texts are available in the vcpkg
ports tree at https://github.com/microsoft/vcpkg.

---

## Reporting issues

If you spot a missing attribution or believe a license is being
infringed in our distribution archives, please open an issue at
https://github.com/DroneDB/DroneDB/issues so we can address it
promptly.
