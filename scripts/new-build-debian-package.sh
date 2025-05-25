#!/usr/bin/env bash
set -euo pipefail

# DroneDB Debian package build script (CI)
#
# Changes in this revision
#   • debhelper‑compat pinned to **13** (per upstream request).
#   • libddb has an explicit SONAME version (libddb.so.0) and symlink chain.
#   • rpaths stripped from all binaries/libraries, fixing dpkg‑shlibdeps errors
#     with libdbus‑1.so.3 and other vcpkg artefacts.
#   • patchelf + chrpath added to build deps.

################################################################################
# 1. Version detection                                                          #
################################################################################

if [[ -z "${DDB_VERSION:-}" ]]; then
  EXACT_TAG="$(git describe --tags --exact-match 2>/dev/null || true)"
  if [[ -n "${EXACT_TAG}" ]]; then
    DDB_VERSION="${EXACT_TAG#v}"
  else
    BASE_VERSION="$(grep -o '"version": "[^\"]*' package.json | cut -d'"' -f4)"
    DDB_VERSION="${BASE_VERSION:-0.0.0}-dirty"
  fi
fi

readonly VCPKG_HOST_TRIPLET="${VCPKG_HOST_TRIPLET:-x64-linux-release}"
export DEBIAN_FRONTEND=noninteractive

echo "==> Building DroneDB ${DDB_VERSION}  (triplet ${VCPKG_HOST_TRIPLET})"

################################################################################
# 2. Build‑time dependencies                                                   #
################################################################################

sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential devscripts lintian debhelper-compat \
    patchelf chrpath libdbus-1-3

################################################################################
# 3. Generate debian/ skeleton                                                 #
################################################################################

rm -rf debian && mkdir -p debian/source

# -------- debian/control ------------------------------------------------------
cat > debian/control <<'EOF'
Source: ddb
Section: science
Priority: optional
Maintainer: DroneDB Team <support@dronedb.app>
Build-Depends: debhelper-compat (= 13), patchelf, chrpath, libdbus-1-3
Standards-Version: 4.7.2
Homepage: https://github.com/DroneDB/DroneDB
Rules-Requires-Root: no

Package: ddb
Architecture: amd64
Depends: ${shlibs:Depends}, ${misc:Depends}, libgl1, libx11-6, libxcb1, libxcb-glx0, libx11-xcb1, libegl1, libxcb-icccm4, libxcb-image0, libxcb-shm0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-sync1, libxcb-xfixes0, libxcb-xinerama0, libxcb-xkb1, libxcb-xinput0, libxkbcommon-x11-0, libxkbcommon0, gdal-data, libdbus-1-3
Description: Effortless aerial data management and sharing
 DroneDB is a toolset for managing and sharing aerial datasets. It extracts
 metadata from aerial images such as GPS location, altitude and gimbal
 orientation parameters, and it can serve the data efficiently over the
 network.
EOF

# -------- copyright -----------------------------------------------------------
cat > debian/copyright <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ddb
Upstream-Contact: DroneDB Team <support@dronedb.app>
Source: https://github.com/DroneDB/DroneDB

Files: *
Copyright: $(date +%Y) DroneDB Team
License: ISC
EOF

# -------- changelog -----------------------------------------------------------
cat > debian/changelog <<EOF
ddb (${DDB_VERSION}-1) unstable; urgency=medium

  * CI build

 -- DroneDB Team <support@dronedb.app>  $(date -R)
EOF

# -------- maintainer scripts --------------------------------------------------
cat > debian/postinst <<'EOF'
#!/bin/sh
set -e
ldconfig
cat > /etc/profile.d/ddb.sh <<EOP
export GDAL_DATA=/usr/share/gdal
export PROJ_LIB=/usr/share/ddb/proj.db
EOP
chmod 644 /etc/profile.d/ddb.sh
#DEBHELPER#
exit 0
EOF
chmod +x debian/postinst

cat > debian/prerm <<'EOF'
#!/bin/sh
set -e
rm -f /etc/profile.d/ddb.sh || true
#DEBHELPER#
exit 0
EOF
chmod +x debian/prerm

# -------- rules ---------------------------------------------------------------
cat > debian/rules <<'EOF'
#!/usr/bin/make -f
export DH_VERBOSE=1
export DEB_BUILD_MAINT_OPTIONS=hardening=+all

%:
	dh $@

# ----------------------------------------------------------------------
# Skip the debug-sections optimiser: our binaries are already stripped
override_dh_dwz:
	@echo "→ Skipping dh_dwz (no debug info present)"

# Nothing to configure/build/test - CI supplies artefacts.
override_dh_auto_configure override_dh_auto_build override_dh_auto_test:
	@echo "⇒ Skipping upstream build steps (pre-built binaries)"

# Install files and fix RPATH / SONAME.
override_dh_auto_install:
	install -Dm755 $(CURDIR)/build/ddbcmd \
		debian/ddb/usr/bin/ddb
	# ---- libraries ------------------------------------------------------
	install -Dm644 $(CURDIR)/build/libddb.so \
		debian/ddb/usr/lib/libddb.so.0
	ln -sfr libddb.so.0 debian/ddb/usr/lib/libddb.so
	install -Dm644 $(CURDIR)/build/libnxs.so \
		debian/ddb/usr/lib/libnxs.so
	install -Dm644 $(CURDIR)/build/vcpkg_installed/${VCPKG_HOST_TRIPLET}/lib/libpdalcpp.so.18 \
		debian/ddb/usr/lib/libpdalcpp.so.18
	ln -sfr libpdalcpp.so.18 debian/ddb/usr/lib/libpdalcpp.so
	install -d debian/ddb/usr/lib/pdal/plugins
	install -m644 $(CURDIR)/build/vcpkg_installed/${VCPKG_HOST_TRIPLET}/lib/libpdal_plugin_*.so.18 \
		debian/ddb/usr/lib/pdal/plugins/ || true

	# ---- data -----------------------------------------------------------
	install -Dm644 $(CURDIR)/build/proj.db \
		debian/ddb/usr/share/ddb/proj.db
	install -Dm644 $(CURDIR)/build/timezone21.bin \
		debian/ddb/usr/share/ddb/timezone21.bin
	install -Dm644 $(CURDIR)/build/sensor_data.sqlite \
		debian/ddb/usr/share/ddb/sensor_data.sqlite
	install -Dm644 $(CURDIR)/build/curl-ca-bundle.crt \
		debian/ddb/usr/share/ddb/curl-ca-bundle.crt

	# ---- fix SONAME & strip RPATH ---------------------------------------
	patchelf --set-soname libddb.so.0 debian/ddb/usr/lib/libddb.so.0
	for f in debian/ddb/usr/bin/ddb debian/ddb/usr/lib/*.so debian/ddb/usr/lib/pdal/plugins/*.so*; do \
		xx="$(basename "$f")"; echo "   · cleaning RPATH on $xx"; chrpath -d "$f" 2>/dev/null || true; \
	done

# libpdalcpp shipped privately - ignore for shlibdeps.
override_dh_shlibdeps:
	dh_shlibdeps -- -xlibpdalcpp.so.18
EOF
chmod +x debian/rules

echo "3.0 (quilt)" > debian/source/format

################################################################################
# 4. Build the package                                                         #
################################################################################

dpkg-buildpackage -us -uc -b

mkdir -p build/package
mv ../ddb_*_amd64.deb build/package/

################################################################################
# 5. Lint & smoke tests                                                       #
################################################################################

lintian -Evi build/package/ddb_*_amd64.deb || true
chmod +x scripts/test-debian-package.sh
./scripts/test-debian-package.sh build/package/ddb_*_amd64.deb

echo "\n✅ DroneDB Debian package build completed"
