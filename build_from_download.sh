#!/usr/bin/env bash
set -euo pipefail

ZIP_NAME="QtWaveshareUsbCan_usbcanb_ready_v2.zip"
ZIP_PATH="$HOME/Downloads/$ZIP_NAME"
WORKDIR="$HOME/Projects/Qt/qtwaveshare_usbcan"

mkdir -p "$WORKDIR"
cd "$WORKDIR"

echo "[*] working dir: $PWD"
echo "[*] zip: $ZIP_PATH"

if [[ ! -f "$ZIP_PATH" ]]; then
    echo "[!] ZIP not found: $ZIP_PATH"
    echo "    Please download $ZIP_NAME into ~/Downloads first."
    exit 1
fi

echo "[*] cleaning old project files"
rm -rf build src examples CMakeLists.txt README.md .gitignore

echo "[*] unpacking"
unzip -o "$ZIP_PATH"

echo "[*] installing build dependencies if needed"
sudo dnf install -y cmake ninja-build gcc-c++ libusb1-devel pkgconf-pkg-config

echo "[*] configuring"
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

echo "[*] building"
cmake --build build

echo
echo "OK. Run examples:"
echo "  ./build/usbcanb_test --init-only"
echo "  ./build/usbcanb_test --self-test --count 10"
echo "  ./build/usbcanb_test --count 10"
