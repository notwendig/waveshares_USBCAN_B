#!/usr/bin/env bash
set -euo pipefail

# QUsbCanB clean POSIX lowlevel build + install helper.
#
# Normal use after unpacking the ZIP:
#   cd ~/Projects/Qt/qtwaveshare_usbcan
#   unzip -o ~/Downloads/QUsbCanB_POSIX_lowlevel_v15_separate_lib_install.zip
#   ./unzip-build.sh
#
# Optional explicit unzip:
#   ./unzip-build.sh --unzip ~/Downloads/QUsbCanB_POSIX_lowlevel_v15_separate_lib_install.zip
#   ./unzip-build.sh /path/to/project --unzip /path/to/archive.zip
#
# Default lowlevel install directory:
#   ~/lib
#
# Installed files:
#   ~/lib/libqusbcanb_lowlevel.so
#   ~/lib/libqusbcanb_lowlevel.so.0
#   ~/lib/libqusbcanb_lowlevel.so.<version>
#   ~/lib/libqusbcanb_lowlevel.a
#   ~/lib/include/qusbcanb_lowlevel.h
#   ~/lib/pkgconfig/qusbcanb_lowlevel.pc

PROJECT_DIR="$PWD"
ZIP_FILE=""
INSTALL_DIR="${HOME}/lib"
RUN_TESTS=1

usage() {
    cat <<USAGE
Usage:
  ./unzip-build.sh [PROJECT_DIR] [--unzip ZIP] [--install-dir DIR] [--no-tests]

Examples:
  ./unzip-build.sh
  ./unzip-build.sh --unzip ~/Downloads/QUsbCanB_POSIX_lowlevel_v15_separate_lib_install.zip
  ./unzip-build.sh --install-dir ~/lib
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --unzip)
            shift
            ZIP_FILE="${1:-}"
            ;;
        --install-dir)
            shift
            INSTALL_DIR="${1:-}"
            ;;
        --no-tests)
            RUN_TESTS=0
            ;;
        *)
            PROJECT_DIR="$1"
            ;;
    esac
    shift || true
done

jobs_count() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 2
    fi
}

expand_path() {
    local p="$1"
    p="${p/#\~/$HOME}"
    printf '%s\n' "$p"
}

INSTALL_DIR="$(expand_path "$INSTALL_DIR")"

printf '[*] project: %s\n' "$PROJECT_DIR"
printf '[*] lowlevel install dir: %s\n' "$INSTALL_DIR"
mkdir -p "$PROJECT_DIR"
cd "$PROJECT_DIR"

if [[ -n "$ZIP_FILE" ]]; then
    ZIP_FILE="$(expand_path "$ZIP_FILE")"
    if [[ ! -f "$ZIP_FILE" ]]; then
        printf '[!] zip not found: %s\n' "$ZIP_FILE" >&2
        exit 1
    fi
    printf '[*] unzip: %s\n' "$ZIP_FILE"
    unzip -o "$ZIP_FILE"
fi

if [[ ! -f CMakeLists.txt ]]; then
    echo '[!] CMakeLists.txt not found. Run inside the project root.' >&2
    exit 1
fi

for tool in cmake pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf '[!] %s not found\n' "$tool" >&2
        exit 1
    fi
done

if ! pkg-config --exists libusb-1.0; then
    echo '[!] libusb-1.0 development package not found via pkg-config' >&2
    echo '[!] Fedora: sudo dnf install libusb1-devel pkgconf-pkg-config cmake gcc-c++ make ninja-build' >&2
    exit 1
fi

if grep -R 'QString\|QByteArray\|QList\|QCanBusFrame' -n src/qusbcanb_lowlevel.h src/qusbcanb_lowlevel.cpp >/dev/null 2>&1; then
    echo '[!] Qt reference found in qusbcanb_lowlevel.*; lowlevel must stay Qt-free.' >&2
    grep -R 'QString\|QByteArray\|QList\|QCanBusFrame' -n src/qusbcanb_lowlevel.h src/qusbcanb_lowlevel.cpp >&2 || true
    exit 1
fi

if grep -R 'open(void\|writeFrames\|readFrames\|configureAndStart\|configureBothAndStart' -n src/qusbcanb_lowlevel.h >/dev/null 2>&1; then
    echo '[!] stale compatibility API found in qusbcanb_lowlevel.h' >&2
    grep -R 'open(void\|writeFrames\|readFrames\|configureAndStart\|configureBothAndStart' -n src/qusbcanb_lowlevel.h >&2 || true
    exit 1
fi

echo '[*] clean build'
rm -rf build
mkdir -p build

cmake -S . -B build \
    -DQUSBCANB_LOWLEVEL_INSTALL_DIR="$INSTALL_DIR"
cmake --build build -j "$(jobs_count)"

echo '[*] install lowlevel libraries'
cmake --install build --component lowlevel

echo '[*] installed lowlevel files'
ls -l "$INSTALL_DIR"/libqusbcanb_lowlevel.*
ls -l "$INSTALL_DIR"/include/qusbcanb_lowlevel.h
ls -l "$INSTALL_DIR"/pkgconfig/qusbcanb_lowlevel.pc

if [[ "$RUN_TESTS" == "1" ]]; then
    echo '[*] regression --count 1'
    ./build/qusbcanb_regression --count 1

    echo '[*] regression --count 100'
    ./build/qusbcanb_regression --count 100
fi

echo '[*] usage from another project:'
printf '    export PKG_CONFIG_PATH="%s/pkgconfig:${PKG_CONFIG_PATH:-}"\n' "$INSTALL_DIR"
printf '    export LD_LIBRARY_PATH="%s:${LD_LIBRARY_PATH:-}"\n' "$INSTALL_DIR"
printf '    c++ your_test.cpp $(pkg-config --cflags --libs qusbcanb_lowlevel) -o your_test\n'

echo '[*] done'
