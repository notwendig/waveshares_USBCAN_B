#include <qusbcanb_lowlevel.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

qusbcanb::Channel parseChannel(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--channel" || arg == "-c") && i + 1 < argc)
            return std::string(argv[++i]) == "2" ? qusbcanb::Channel::Can2 : qusbcanb::Channel::Can1;
    }
    return qusbcanb::Channel::Can1;
}

bool hasFlag(int argc, char **argv, const std::string &flag)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    const auto channel = parseChannel(argc, argv);
    const bool selfTest = hasFlag(argc, argv, "--self-test");
    const auto mode = selfTest ? qusbcanb::CanMode::SelfTest : qusbcanb::CanMode::Normal;

    const auto ep = qusbcanb::LowLevelDevice::endpoints(channel);
    std::cout << qusbcanb::channelName(channel)
              << ": CMD OUT 0x" << std::hex << int(ep.command_out)
              << " CMD IN 0x" << int(ep.command_in)
              << " MSG OUT 0x" << int(ep.message_out)
              << " MSG IN 0x" << int(ep.message_in) << std::dec << '\n';

    qusbcanb::LowLevelDevice dev;
    if (!dev.open()) {
        std::cerr << "open failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    if (!dev.configure(channel, 125000, mode)) {
        std::cerr << "configure failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    qusbcanb::CanFrame frame;
    frame.id = channel == qusbcanb::Channel::Can1 ? 0x123 : 0x321;
    frame.dlc = 8;
    frame.data = {0x51, 0x55, 0x53, 0x42, 0x43, 0x41, 0x4e, 0x42};

    if (!dev.send(channel, frame)) {
        std::cerr << "send failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    std::vector<qusbcanb::CanFrame> rx;
    if (!dev.receive(channel, rx, 64, std::chrono::milliseconds{100})) {
        std::cerr << "receive failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "sent 1 frame, received " << rx.size() << " frame(s) on same channel\n";
    for (const auto &received : rx)
        std::cout << "  " << qusbcanb::frameToCandumpString(received) << '\n';

    (void)dev.stop(channel);
    return EXIT_SUCCESS;
}
