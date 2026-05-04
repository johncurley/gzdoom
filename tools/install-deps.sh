#!/bin/bash
# GZDoom Dependency Installer for Linux and BSD

OS=$(uname -s)

echo "Detected OS: $OS"

case "$OS" in
    "Linux")
        if [ -f /etc/debian_version ]; then
            echo "Detected Debian/Ubuntu-based system"
            sudo apt update && sudo apt install -y libx11-dev libxext-dev libxinerama-dev libxrandr-dev libgl1-mesa-dev libdispatch-dev pkg-config cmake ninja-build g++
        elif [ -f /etc/fedora-release ]; then
            echo "Detected Fedora-based system"
            sudo dnf install -y libX11-devel libXext-devel libXinerama-devel libXrandr-devel mesa-libGL-devel libdispatch-devel pkgconf-pkg-config cmake ninja-build gcc-c++
        elif [ -f /etc/arch-release ]; then
            echo "Detected Arch-based system"
            sudo pacman -Sy --needed libx11 libxext libxinerama libxrandr mesa libdispatch pkgconf cmake ninja gcc
        else
            echo "Unknown Linux distribution. Please install X11, OpenGL, and libdispatch development packages manually."
        fi
        ;;
    "FreeBSD")
        echo "Detected FreeBSD"
        sudo pkg install xorgproto xisb libX11 libXext libXinerama libXrandr mesa-libs libdispatch pkgconf cmake ninja
        ;;
    "OpenBSD")
        echo "Detected OpenBSD"
        # X11 is usually in base, but we need pkgconf and libdispatch
        sudo pkg_add pkgconf libdispatch cmake ninja
        ;;
    "Haiku")
        echo "Detected HaikuOS"
        # Since you are developing on the platform, these are the typical deps
        pkgman install libx11_devel mesa_devel libdispatch_devel cmake ninja
        ;;
    *)
        echo "Unsupported OS: $OS. Please install dependencies manually."
        exit 1
        ;;
esac

echo "Dependency installation complete."
