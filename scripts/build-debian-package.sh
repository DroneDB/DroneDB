#!/bin/bash
set -e

# This script builds a Debian package for DroneDB
# It's designed to run as part of the CI pipeline
# Assumes the application is already built in the build folder

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

# Install Debian packaging tools
echo "Installing Debian packaging tools..."
sudo apt-get update
sudo apt-get install -y debhelper devscripts fakeroot lintian

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
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.5.0
Homepage: https://github.com/DroneDB/DroneDB

Package: ddb
Architecture: any
Depends: \${shlibs:Depends}, \${misc:Depends}, libgl1, libx11-6, libxcb1, libxcb-glx0, libx11-xcb1, libegl1, libxcb-icccm4, libxcb-image0, libxcb-shm0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-sync1, libxcb-xfixes0, libxcb-xinerama0, libxcb-xkb1, libxcb-xinput0, libxkbcommon-x11-0, libxkbcommon0
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

# Create postinst script for environment setup
echo "Creating postinst script..."
cat > debian/postinst << EOF
#!/bin/sh
set -e

# Create symbolic links if needed
ldconfig

# Set environment variables by creating a config file
cat > /etc/profile.d/ddb.sh << EOS
export DDB_PROJ_PATH=/usr/share/ddb/proj.db
export DDB_TIMEZONE_PATH=/usr/share/ddb/timezone21.bin
export DDB_SENSOR_DATA_PATH=/usr/share/ddb/sensor_data.sqlite
export CURL_CA_BUNDLE_PATH=/usr/share/ddb/curl-ca-bundle.crt
EOS

chmod 644 /etc/profile.d/ddb.sh

#DEBHELPER#
exit 0
EOF

# Make postinst executable
chmod +x debian/postinst

# Create prerm script for cleanup
echo "Creating prerm script..."
cat > debian/prerm << EOF
#!/bin/sh
set -e

if [ -f /etc/profile.d/ddb.sh ]; then
    rm -f /etc/profile.d/ddb.sh
fi

#DEBHELPER#
exit 0
EOF

# Make prerm executable
chmod +x debian/prerm

# Create changelog
echo "Creating changelog file..."
cat > debian/changelog << EOF
ddb (${DDB_VERSION}) unstable; urgency=medium

  * New release version ${DDB_VERSION}

 -- DroneDB Team <support@dronedb.app>  $(date -R)
EOF

# Create rules file that skips the build step
echo "Creating rules file..."
cat > debian/rules << EOF
#!/usr/bin/make -f

%:
	dh \$@

# Skip the configure step since we're using pre-built binaries
override_dh_auto_configure:
	# Do nothing, skip configure

# Skip the build step since we're using pre-built binaries
override_dh_auto_build:
	# Do nothing, skip build

# Skip the test step
override_dh_auto_test:
	# Do nothing, skip tests

override_dh_shlibdeps:
	dh_shlibdeps --dpkg-shlibdeps-params=--ignore-missing-info

override_dh_auto_install:
	# Create directory structure for installed files
	mkdir -p debian/ddb/usr/bin
	mkdir -p debian/ddb/usr/lib
	mkdir -p debian/ddb/usr/share/ddb
    
	# Copy binary and libs from build directory directly
	cp \$(CURDIR)/build/ddbcmd debian/ddb/usr/bin/ddb
	cp \$(CURDIR)/build/libddb.so debian/ddb/usr/lib/
	cp \$(CURDIR)/build/libnxs.so debian/ddb/usr/lib/
    
	# Copy PDAL libraries from vcpkg installed directory
	cp \$(CURDIR)/build/vcpkg_installed/x64-linux-release/lib/libpdalcpp.so.18 debian/ddb/usr/lib/
	cp \$(CURDIR)/build/vcpkg_installed/x64-linux-release/lib/libdbus-1.so.3 debian/ddb/usr/lib/
    
	# Create necessary symbolic links for libraries
	ln -sf libpdalcpp.so.18 debian/ddb/usr/lib/libpdalcpp.so
	ln -sf libdbus-1.so.3 debian/ddb/usr/lib/libdbus-1.so
    
	# Copy PDAL plugins if needed
	mkdir -p debian/ddb/usr/lib/pdal/plugins
	cp \$(CURDIR)/build/vcpkg_installed/x64-linux-release/lib/libpdal_plugin_*.so.18 debian/ddb/usr/lib/pdal/plugins/ || true
    
	# Copy data files from build directory directly
	cp \$(CURDIR)/build/proj.db debian/ddb/usr/share/ddb/
	cp \$(CURDIR)/build/timezone21.bin debian/ddb/usr/share/ddb/
	cp \$(CURDIR)/build/sensor_data.sqlite debian/ddb/usr/share/ddb/
	cp \$(CURDIR)/build/curl-ca-bundle.crt debian/ddb/usr/share/ddb/
    
	# Set permissions
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
