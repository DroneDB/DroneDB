#!/bin/bash
set -e

# This script builds a Debian package for DroneDB
# It's designed to run as part of the CI pipeline

# Set necessary environment variables
echo "Setting up environment variables..."
if [ -z "$DDB_VERSION" ]; then
  # If not in a tag, use the git describe output
  DDB_VERSION=$(git describe --tags --abbrev=0 | sed 's/^v//')
  if [ -z "$DDB_VERSION" ]; then
    # Fallback to package.json version if no tag is available
    DDB_VERSION=$(grep -o '"version": "[^"]*' package.json | cut -d'"' -f4)
  fi
fi

echo "Building DroneDB Debian package version: $DDB_VERSION"
export DDB_VERSION
export DEBIAN_FRONTEND=noninteractive

# Ensure git submodules are initialized
echo "Ensuring git submodules are initialized..."
git submodule update --init --recursive

# Install Debian packaging tools
echo "Installing Debian packaging tools..."
sudo apt-get update
sudo apt-get install -y build-essential cmake debhelper devscripts fakeroot lintian

# Install requirements
sudo apt-get install -y libgdal-dev libspatialite-dev libexiv2-dev libzip-dev libpng-dev zlib1g-dev libssl-dev libcurl4-openssl-dev

# Create debian packaging structure
echo "Creating debian packaging structure..."
mkdir -p debian/source

# Create control file with dependencies based on the project requirements
echo "Creating control file..."
cat > debian/control << EOF
Source: ddb
Section: science
Priority: optional
Maintainer: DroneDB Team <support@dronedb.app>
Build-Depends: debhelper-compat (= 13), cmake, build-essential, 
               libgdal-dev, libsqlite3-dev, libspatialite-dev, 
               libexiv2-dev, libzip-dev, libpng-dev, zlib1g-dev,
               libssl-dev, libcurl4-openssl-dev
Standards-Version: 4.5.0
Homepage: https://github.com/DroneDB/DroneDB

Package: ddb
Architecture: any
Depends: \${shlibs:Depends}, \${misc:Depends},
         libgdal30, libsqlite3-0, libspatialite7, 
         libexiv2-27, libzip4, libpng16-16, zlib1g,
         libssl3, libcurl4
Description: Effortless aerial data management and sharing
 DroneDB is a toolset for effortlessly managing and sharing aerial datasets.
 It can extract metadata from aerial images such as GPS location, altitude,
 gimbal orientation parameters and more. It can also efficiently share data 
 over the network.
EOF

# Create copyright file
echo "Creating copyright file..."
cat > debian/copyright << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ddb
Upstream-Contact: DroneDB Team <support@dronedb.app>
Source: https://github.com/DroneDB/DroneDB

Files: *
Copyright: $(date +%Y) DroneDB Team
License: ISC License
EOF

# Create changelog
echo "Creating changelog file..."
cat > debian/changelog << EOF
ddb (${DDB_VERSION}) unstable; urgency=medium

  * New release version ${DDB_VERSION}

 -- DroneDB Team <support@dronedb.app>  $(date -R)
EOF

# Create rules file with vcpkg integration (using existing submodule)
echo "Creating rules file..."
cat > debian/rules << EOF
#!/usr/bin/make -f

%:
	dh \$@

override_dh_auto_configure:
	# Ensure vcpkg submodule is initialized and bootstrap is run
	if [ ! -f \$(CURDIR)/vcpkg/vcpkg ]; then \
		echo "Bootstrapping vcpkg..."; \
		\$(CURDIR)/vcpkg/bootstrap-vcpkg.sh -disableMetrics; \
	fi
	export VCPKG_ROOT=\$(CURDIR)/vcpkg && \
	dh_auto_configure -- -DVCPKG_OVERLAY_TRIPLETS=\$(CURDIR)/vcpkg-triplets -DVCPKG_HOST_TRIPLET=x64-linux-release -DVCPKG_TARGET_TRIPLET=x64-linux-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=\$(CURDIR)/vcpkg/scripts/buildsystems/vcpkg.cmake

override_dh_shlibdeps:
	dh_shlibdeps --dpkg-shlibdeps-params=--ignore-missing-info

override_dh_auto_install:
	dh_auto_install
	# Create directory structure for installed files
	mkdir -p debian/ddb/usr/bin
	mkdir -p debian/ddb/usr/lib
	# Copy binary and libs
	cp \$(CURDIR)/build/ddbcmd debian/ddb/usr/bin/ddb
	cp \$(CURDIR)/build/*.so* debian/ddb/usr/lib/ || true
	cp \$(CURDIR)/build/*.dll debian/ddb/usr/lib/ || true
	chmod +x debian/ddb/usr/bin/ddb
EOF

# Make rules executable
chmod +x debian/rules

# Create source/format
echo "3.0 (quilt)" > debian/source/format

# Build the Debian package
echo "Building Debian package..."
debuild -us -uc -b

# Move the package to a predictable location
echo "Moving package to build directory..."
mkdir -p build/package
mv ../ddb_*.deb build/package/

# Test the package
echo "Testing the Debian package..."
chmod +x scripts/test-debian-package.sh
./scripts/test-debian-package.sh build/package/ddb_*.deb

echo "Debian package build complete!"
ls -la build/package/
