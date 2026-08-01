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
