#!/bin/bash
set -e

# Script to test the installation of the DroneDB Debian package
# This is useful for CI/CD or local testing to verify the package installs and works correctly

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <path_to_deb_file>"
    exit 1
fi

DEB_FILE=$1

if [ ! -f "$DEB_FILE" ]; then
    echo "Error: Debian package file '$DEB_FILE' not found!"
    exit 1
fi

echo "Testing installation of Debian package: $DEB_FILE"

# Create a temporary directory for testing
TEMP_DIR=$(mktemp -d)
echo "Using temporary directory: $TEMP_DIR"

# Clean up on exit
function cleanup {
    echo "Cleaning up temporary directory..."
    rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

# Extract package to temp directory to test its contents
dpkg-deb -x "$DEB_FILE" "$TEMP_DIR"

echo "Checking package contents..."
echo "Checking binary..."
ls -la "$TEMP_DIR/usr/bin/"

echo "Checking libraries..."
ls -la "$TEMP_DIR/usr/lib/" || echo "No libraries directory found"

echo "Checking shared data files..."
ls -la "$TEMP_DIR/usr/share/ddb/" || echo "No share directory found"

# Check if the binary exists
if [ ! -f "$TEMP_DIR/usr/bin/ddb" ]; then
    echo "Error: ddb binary not found in package!"
    exit 1
fi

echo "Binary found, checking if it's executable..."
if [ ! -x "$TEMP_DIR/usr/bin/ddb" ]; then
    echo "Error: ddb binary is not executable!"
    exit 1
fi

# Check for required libraries
echo "Checking for required libraries..."
if [ ! -f "$TEMP_DIR/usr/lib/libddb.so" ]; then
    echo "Error: libddb.so not found in package!"
    exit 1
fi

if [ ! -f "$TEMP_DIR/usr/lib/libnxs.so" ]; then
    echo "Error: libnxs.so not found in package!"
    exit 1
fi

# Check for required data files
echo "Checking for required data files..."
for file in proj.db timezone21.bin sensor_data.sqlite curl-ca-bundle.crt; do
    if [ ! -f "$TEMP_DIR/usr/share/ddb/$file" ]; then
        echo "Error: $file not found in package!"
        exit 1
    fi
done

# Obj2Tiles is optional, but when shipped it must actually start: it is a .NET
# single-file app whose appended bundle is destroyed by strip/dwz, and the only
# symptom is a runtime "Failure processing application bundle" error.
if [ -f "$TEMP_DIR/usr/bin/Obj2Tiles" ]; then
    echo "Checking Obj2Tiles..."
    if ! OBJ2TILES_OUTPUT=$("$TEMP_DIR/usr/bin/Obj2Tiles" --version 2>&1); then
        echo "Error: packaged Obj2Tiles failed to run!"
        echo "$OBJ2TILES_OUTPUT"
        exit 1
    fi
    echo "  $OBJ2TILES_OUTPUT"

    # Without libktx next to the binary, --texture-format Ktx2 aborts the whole run.
    if [ ! -f "$TEMP_DIR/usr/bin/libktx.so" ]; then
        echo "Error: libktx.so not found next to Obj2Tiles; KTX2 texture compression would fail!"
        exit 1
    fi
fi

# PDAL >= 19 resolves stage plugins with dlopen() of the UNVERSIONED soname
# (libpdal_plugin_reader_e57.so) from the loader path only - the directory
# containing libpdalcpp. It does not scan the plugins subdirectory, so a
# package that ships just the versioned files under /usr/lib/pdal/plugins
# indexes zero E57 points ("Cannot create reader stage readers.e57") even
# though file-listing checks look complete. The unversioned copies must ship
# next to libpdalcpp in /usr/lib. The runtime counterpart of this check (a
# real E57 add against the installed package) runs as the docker-based
# "package smoke test" step in .github/workflows/c-cpp.yml: an extraction-
# only smoke test cannot be trusted here because the build tree is reachable
# through the binaries' RUNPATH and would shadow the packaged files.
echo "Checking PDAL plugin packaging..."
if [ ! -f "$TEMP_DIR/usr/lib/libpdalcpp.so.20" ] && ! ls "$TEMP_DIR"/usr/lib/libpdalcpp.so.* >/dev/null 2>&1; then
    echo "Error: no versioned libpdalcpp.so.* found in /usr/lib!"
    exit 1
fi
if [ ! -f "$TEMP_DIR/usr/lib/libpdal_plugin_reader_e57.so" ]; then
    echo "Error: unversioned libpdal_plugin_reader_e57.so not found next to libpdalcpp!"
    echo "PDAL >= 19 would fail every E57 add with 'Cannot create reader stage readers.e57'."
    exit 1
fi
if [ ! -d "$TEMP_DIR/usr/lib/pdal/plugins" ] || ! ls "$TEMP_DIR"/usr/lib/pdal/plugins/libpdal_plugin_reader_e57.so.* >/dev/null 2>&1; then
    echo "Error: versioned PDAL plugins missing from /usr/lib/pdal/plugins!"
    exit 1
fi

# Check postinst script
echo "Checking postinst script..."
if [ ! -f "$TEMP_DIR/DEBIAN/postinst" ]; then
    echo "Warning: postinst script not found in extracted package. This is normal for dpkg-deb extraction."
fi

echo "Everything looks good! The Debian package contains the necessary files."
echo ""
echo "To install the package on a Debian/Ubuntu system, run:"
echo "  sudo apt install ./$DEB_FILE"
echo ""
echo "Once installed, test the application with:"
echo "  ddb version --debug"
