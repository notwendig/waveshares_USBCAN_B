#include <qusbcanb_lowlevel.h>

#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    const qusbcanb::Channel channel = (argc > 1 && std::string(argv[1]) == "2")
        ? qusbcanb::Channel::Can2
        : qusbcanb::Channel::Can1;

    qusbcanb::LowLevelDevice dev;
    if (!dev.open()) {
        std::cerr << "open failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    if (!dev.configure(channel, 125000, qusbcanb::CanMode::Normal)) {
        std::cerr << "configure " << qusbcanb::channelName(channel)
                  << " failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    std::vector<qusbcanb::CanFrame> frames;
    if (!dev.receive(channel, frames, 64, std::chrono::milliseconds{100})) {
        std::cerr << "receive failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    for (const auto &frame : frames)
        std::cout << qusbcanb::frameToCandumpString(frame) << '\n';

    (void)dev.stop(channel);
    return EXIT_SUCCESS;
}
