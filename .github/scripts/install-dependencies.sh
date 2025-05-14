#!/bin/bash
# Script to install dependencies across all GitHub workflows
# Using the same package list as in Dockerfile-builder

set -e

# Common packages for all Ubuntu/Debian-based environments
install_ubuntu_packages() {
    sudo apt-get update
    sudo apt-get install -y \
        apt-utils git cmake unzip tar build-essential flex zip curl \
        bison pkg-config python3 python3-jinja2 python3-setuptools autoconf automake libtool \
        libreadline-dev libssl-dev lbzip2 libbz2-dev zlib1g-dev libffi-dev liblzma-dev \
        '^libxcb.*-dev' libx11-xcb-dev libgl1-mesa-dev libxrender-dev \
        libxi-dev libxkbcommon-dev libxkbcommon-x11-dev libxtst-dev libltdl-dev
}

# Install Python packages needed for builds
install_python_packages() {
    pip install exodus-bundler jinja2
    
    # Add other common Python packages needed across workflows
    if [ "$1" = "docs" ]; then
        pip install sphinx sphinx_rtd_theme breathe exhale
    fi
}

# Install macOS dependencies via Homebrew
install_macos_packages() {
    brew install flex bison autoconf
}

# Main logic to determine platform and install appropriate packages
case "$(uname -s)" in
    Linux*)
        install_ubuntu_packages
        install_python_packages "$1"
        ;;
    Darwin*)
        install_macos_packages
        install_python_packages "$1"
        ;;
    *)
        echo "Unsupported platform"
        exit 1
        ;;
esac

exit 0
