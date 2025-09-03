#!/bin/sh
set -e
__dirname=$(cd $(dirname "$0"); pwd -P)
cd "${__dirname}"

# DroneDB for Linux installation script
#
# See https://docs.dronedb.app for more information.
#
# This script is meant for quick & easy install via:
#   $ curl -fsSL https://get.dronedb.app -o get-ddb.sh
#   $ sh get-ddb.sh
#
# NOTE: Make sure to verify the contents of the script
#       you downloaded matches the contents of install.sh
#       located at https://github.com/DroneDB/DroneDB/scripts/linux_install_script.sh
#       before executing.
#

LATEST_RELEASE="https://github.com/DroneDB/DroneDB/releases/download/###RELEASE_TAG###/ddb_###RELEASE_VERSION###_amd64.deb"

command_exists() {
	command -v "$@" > /dev/null 2>&1
}

do_install() {
	echo "# Executing DroneDB install script"

    echo "# Downloading $LATEST_RELEASE..."
    curl -fsSL $LATEST_RELEASE -o /tmp/ddb.deb

    echo "# Installing DEB package..."
    if command_exists sudo; then
        sudo dpkg -i /tmp/ddb.deb || sudo apt-get install -f -y
    else
        echo "Error: sudo is required to install the DEB package"
        exit 1
    fi

    echo "# Cleaning up..."
    if [ -e /tmp/ddb.deb ]; then
        rm /tmp/ddb.deb
    fi

    echo ""
    echo "    ____                        ____  ____ "
    echo "   / __ \_________  ____  ___  / __ \/ __ )"
    echo "  / / / / ___/ __ \/ __ \/ _ \/ / / / __  |"
    echo " / /_/ / /  / /_/ / / / /  __/ /_/ / /_/ / "
    echo "/_____/_/   \____/_/ /_/\___/_____/_____/  "
    echo "                                           "
    echo "Type: ddb --help or visit https://docs.dronedb.app to get started!"
    echo ""
}

# wrapped up in a function so that we have some protection against only getting
# half the file during "curl | sh"
do_install
