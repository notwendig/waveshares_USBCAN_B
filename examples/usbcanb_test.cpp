#include "usbcanb.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static bool hasArg(int argc, char** argv, const std::string& name)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == name) return true;
    }
    return false;
}

static int intArg(int argc, char** argv, const std::string& name, int def)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return std::atoi(argv[i + 1]);
    }
    return def;
}

static UsbCanB::Frame makeFrame(uint32_t id, int seq)
{
    UsbCanB::Frame f{};
    f.id = id;
    f.extended = false;
    f.remote = false;
    f.dlc = 8;
    f.data = {0x11, 0x22, 0x33, 0x44, static_cast<uint8_t>(seq), 0x66, 0x77, 0x88};
    return f;
}

static void dumpRx(int ch, const std::vector<UsbCanB::Frame>& frames)
{
    for (const auto& f : frames) {
        std::cout << "RX CAN" << ch << " id=0x" << std::hex << f.id << std::dec
                  << " dlc=" << static_cast<int>(f.dlc) << " data=";
        for (int i = 0; i < f.dlc; ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(f.data[static_cast<size_t>(i)]) << ' ';
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
    }
}

int main(int argc, char** argv)
{
    const bool initOnly = hasArg(argc, argv, "--init-only");
    const bool selfTest = hasArg(argc, argv, "--self-test");
    const bool can1Only = hasArg(argc, argv, "--can1-only");
    const bool can2Only = hasArg(argc, argv, "--can2-only");
    const bool swapInitOrder = hasArg(argc, argv, "--swap-init-order");
    const bool skipHandshake = hasArg(argc, argv, "--no-handshake");
    const bool compactInit = hasArg(argc, argv, "--compact-init");
    const int count = intArg(argc, argv, "--count", 10);
    const int timeout = intArg(argc, argv, "--timeout", 1000);

    const auto mode = selfTest ? UsbCanB::Mode::SelfTest : UsbCanB::Mode::Normal;
    const auto can1 = UsbCanB::channel1();
    const auto can2 = UsbCanB::channel2();

    std::cout << "usbcanb_test v9: 125 kbit/s, count=" << count
              << ", mode=" << (selfTest ? "self-test" : "normal")
              << ", initLayout=" << (compactInit ? "compact16" : "controlcan64") << '\n';
    std::cout << "CAN1 endpoints: CMD OUT 0x02, TX OUT 0x01, RX IN 0x81\n";
    std::cout << "CAN2 endpoints: CMD OUT 0x04, TX OUT 0x03, RX IN 0x83\n";

    UsbCanB dev;
    if (!dev.open()) {
        std::cerr << dev.lastError() << '\n';
        return 1;
    }
    std::cout << "opened USB-CAN 04d8:0053, interface 0 claimed\n";

    if (!skipHandshake) {
        if (dev.handshake(timeout)) {
            std::cout << "handshake OK/non-fatal\n";
        } else {
            std::cout << "handshake failed/non-fatal: " << dev.lastError() << '\n';
        }
    }

    auto initChannel = [&](const UsbCanB::Channel& ch, const char* name) -> bool {
        if (!dev.clearBuffer(ch, timeout)) {
            std::cerr << name << " clear failed: " << dev.lastError() << '\n';
            return false;
        }
        std::cout << name << " clear OK\n";
        const bool initOk = compactInit ? dev.initCan125kCompact(ch, mode, timeout)
                                        : dev.initCan125k(ch, mode, timeout);
        if (!initOk) {
            std::cerr << name << " init failed: " << dev.lastError() << '\n';
            return false;
        }
        std::cout << name << " init OK\n";
        if (!dev.startCan(ch, timeout)) {
            std::cerr << name << " start failed: " << dev.lastError() << '\n';
            return false;
        }
        std::cout << name << " start OK\n";
        return true;
    };

    bool ok1 = true;
    bool ok2 = true;

    if (swapInitOrder) {
        if (!can1Only) ok2 = initChannel(can2, "CAN2");
        if (!can2Only) ok1 = initChannel(can1, "CAN1");
    } else {
        if (!can2Only) ok1 = initChannel(can1, "CAN1");
        if (!can1Only) ok2 = initChannel(can2, "CAN2");
    }

    if (initOnly) {
        return (ok1 && ok2) ? 0 : 2;
    }
    if (!ok1 && !ok2) return 2;

    int sent1 = 0;
    int sent2 = 0;
    int rxTotal = 0;

    for (int seq = 0; seq < count; ++seq) {
        if (!can2Only && ok1) {
            const std::vector<UsbCanB::Frame> frames{makeFrame(0x121, seq)};
            const int n = dev.transmit(can1, frames, timeout);
            sent1 += n;
            std::cout << "TX CAN1 sent=" << n << " seq=" << seq << '\n';
            if (n == 0) {
                std::cerr << "CAN1 TX stopped: " << dev.lastError() << '\n';
                ok1 = false;
            }
        }

        if (!can1Only && ok2) {
            const std::vector<UsbCanB::Frame> frames{makeFrame(0x122, seq)};
            const int n = dev.transmit(can2, frames, timeout);
            sent2 += n;
            std::cout << "TX CAN2 sent=" << n << " seq=" << seq << '\n';
            if (n == 0) {
                std::cerr << "CAN2 TX stopped: " << dev.lastError() << '\n';
                ok2 = false;
            }
        }

        if (!can2Only && ok1) {
            auto rx = dev.receive(can1, 20);
            rxTotal += static_cast<int>(rx.size());
            dumpRx(1, rx);
        }
        if (!can1Only && ok2) {
            auto rx = dev.receive(can2, 20);
            rxTotal += static_cast<int>(rx.size());
            dumpRx(2, rx);
        }

        if (!ok1 && !ok2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "summary: CAN1 sent=" << sent1 << '/' << count
              << ", CAN2 sent=" << sent2 << '/' << count
              << ", rx=" << rxTotal << '\n';

    return 0;
}
