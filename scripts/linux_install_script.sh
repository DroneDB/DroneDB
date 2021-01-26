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
# TODO: this needs to be updated automatically...

LATEST_RELEASE="https://github.com/DroneDB/DroneDB/releases/download/v0.9.8/ddb-0.9.8-linux.tgz"

command_exists() {
	command -v "$@" > /dev/null 2>&1
}

do_install() {
	echo "# Executing DroneDB install script"

    echo "# Downloading $LATEST_RELEASE..."
    curl -fsSL $LATEST_RELEASE -o /tmp/ddb.tgz

    if [ ! -e ~/.local ]; then
        mkdir -p ~/.local
    fi

    echo "# Extracting..."
    tar -zxf /tmp/ddb.tgz -C ~/.local

    echo "# Setting up..."
    if [ -e ~/.local/exodus ]; then

        echo "# Removing previous version of ddb..."
        if [ -e ~/.local/ddb ]; then
            rm -r ~/.local/ddb
        fi

        mv ~/.local/exodus ~/.local/ddb

        if [ -e ~/.bashrc ]; then
            if test ! $(command_exists ddb); then
                echo "# Adding ~/.local/ddb/bin to PATH in .bashrc"
                echo "export PATH=\$PATH:\$HOME/.local/ddb/bin" >> ~/.bashrc
            fi
        else
            echo "# Program installed in $HOME/.local/ddb/bin. Make sure to add this path to your PATH environment variable."
        fi
    else
        echo "! Cannot install DroneDB: ~/.local/exodus not found. This might be a bug. Please open an issue on https://github.com/DroneDB/DroneDB/issues"
        exit 1
    fi

    echo "# Cleaning up..."
    if [ -e /tmp/ddb.tgz ]; then
        rm /tmp/ddb.tgz
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

    if [ -z "$SHELL" ] && [ -e /bin/bash ]; then
        export SHELL=/bin/bash
    fi

    if [ -e $SHELL ] && [ -e ~/.bashrc ]; then
        $SHELL
    else
        echo "Important! Manually update your PATH to include $HOME/.local/ddb/bin"
    fi
}

# wrapped up in a function so that we have some protection against only getting
# half the file during "curl | sh"
do_install
