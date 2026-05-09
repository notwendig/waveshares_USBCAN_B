# QUsbCanB POSIX/libusb LowLevel separate library

This version keeps the low-level backend completely Qt-free and builds it as a standalone library pair:

- `libqusbcanb_lowlevel.so`
- `libqusbcanb_lowlevel.a`

The default install directory is:

```text
~/lib
```

Installed layout:

```text
~/lib/libqusbcanb_lowlevel.so
~/lib/libqusbcanb_lowlevel.so.0
~/lib/libqusbcanb_lowlevel.so.0.16.0
~/lib/libqusbcanb_lowlevel.a
~/lib/include/qusbcanb_lowlevel.h
~/lib/pkgconfig/qusbcanb_lowlevel.pc
```

## Build, install, test

From the project root:

rm -rf build
cmake -S . -B build -DQUSBCANB_LOWLEVEL_INSTALL_DIR="$HOME/lib"
cmake --build build -j
cmake --install build --component lowlevel
./build/qusbcanb_regression --count 1
./build/qusbcanb_regression --count 100

## LowLevel interface

The public interface is intentionally explicit and Qt-free:

```cpp
#include <qusbcanb_lowlevel.h>

#include <chrono>
#include <iostream>
#include <vector>

int main()
{
    qusbcanb::LowLevelDevice dev;

    if (!dev.open()) {
        std::cerr << dev.lastError() << '\n';
        return 1;
    }

    if (!dev.configureBoth(125000, qusbcanb::CanMode::Normal)) {
        std::cerr << dev.lastError() << '\n';
        return 1;
    }

    qusbcanb::CanFrame frame;
    frame.id = 0x123;
    frame.dlc = 2;
    frame.data[0] = 0x11;
    frame.data[1] = 0x22;

    if (!dev.send(qusbcanb::Channel::Can1, frame)) {
        std::cerr << dev.lastError() << '\n';
        return 1;
    }

    std::vector<qusbcanb::CanFrame> rx;
    if (!dev.receive(qusbcanb::Channel::Can2, rx, 64, std::chrono::milliseconds{10})) {
        std::cerr << dev.lastError() << '\n';
        return 1;
    }

    dev.close();
    return 0;
}
```

## Use installed library from another project

```bash
export PKG_CONFIG_PATH="$HOME/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$HOME/lib:${LD_LIBRARY_PATH:-}"

c++ your_test.cpp $(pkg-config --cflags --libs qusbcanb_lowlevel) -o your_test
./your_test
```

Or manually:

```bash
c++ your_test.cpp -I$HOME/lib/include -L$HOME/lib -lqusbcanb_lowlevel -lusb-1.0 -o your_test
LD_LIBRARY_PATH=$HOME/lib ./your_test
```

## Endpoint layout used by this backend

```text
CAN1: CMD OUT 0x02, CMD IN 0x82, MSG OUT 0x01, MSG IN 0x81
CAN2: CMD OUT 0x04, CMD IN 0x84, MSG OUT 0x03, MSG IN 0x83
```

This is the layout that passed the cross-channel reression test on the connected CAN1 <-> CAN2 setup.
