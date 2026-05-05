#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
echo "[*] project: $PWD"
echo "[*] installing/checking build dependencies"
sudo dnf install -y cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qtserialbus-devel libusb1-devel pkgconf-pkg-config
echo "[*] configure"
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
echo "[*] build"
cmake --build build
cat <<'MSG'

OK. Examples:
  ./build/qusbcanb_test --init-only
  ./build/qusbcanb_test --self-test --count 10
  ./build/qusbcanb_test --count 10
  ./build/qusbcanb_test --channel 2 --init-only

Regression:
  ./build/qusbcanb_regression --self-test --count 10000
  ./build/qusbcanb_regression --count 10000
MSG
