#include "../src/qusbcanb_lowlevel.h"

#include <cstdlib>
#include <iostream>

int main()
{
    qusbcanb::LowLevelDevice dev;
    if (!dev.open()) {
        std::cerr << "open failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    const auto timing = qusbcanb::bitTimingForBitrate(125000);
    if (!dev.reset(qusbcanb::Channel::Can1) ||
        !dev.init(qusbcanb::Channel::Can1, timing) ||
        !dev.start(qusbcanb::Channel::Can1)) {
        std::cerr << "init failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    std::vector<qusbcanb::CanFrame> frames;
    if (dev.receive(qusbcanb::Channel::Can1, frames)) {
        for (const auto &frame : frames)
            std::cout << qusbcanb::frameToCandumpString(frame) << '\n';
    } else {
        std::cerr << "receive failed: " << dev.lastError() << '\n';
    }

    dev.stop(qusbcanb::Channel::Can1);
    return EXIT_SUCCESS;
}
