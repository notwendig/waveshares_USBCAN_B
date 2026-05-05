#include <qusbcanb_lowlevel.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options
{
    std::size_t count = 100;
    std::size_t batch = 48;
    std::uint32_t bitrate = 125000;
    qusbcanb::CanMode mode = qusbcanb::CanMode::Normal;
};

void usage(const char *argv0)
{
    std::cout << "Usage: " << argv0 << " [--count N] [--batch N] [--bitrate N] [--mode normal|listen|self-test]\n";
}

Options parseOptions(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--help") || (arg == "-h")) {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (arg == "--count" && i + 1 < argc) {
            opt.count = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--batch" && i + 1 < argc) {
            opt.batch = std::max<std::size_t>(1, static_cast<std::size_t>(std::stoull(argv[++i])));
        } else if (arg == "--bitrate" && i + 1 < argc) {
            opt.bitrate = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--mode" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "normal")
                opt.mode = qusbcanb::CanMode::Normal;
            else if (mode == "listen" || mode == "listen-only")
                opt.mode = qusbcanb::CanMode::ListenOnly;
            else if (mode == "self-test" || mode == "selftest")
                opt.mode = qusbcanb::CanMode::SelfTest;
            else
                throw std::runtime_error("unknown mode: " + mode);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return opt;
}

const char *modeName(qusbcanb::CanMode mode)
{
    switch (mode) {
    case qusbcanb::CanMode::Normal: return "normal";
    case qusbcanb::CanMode::ListenOnly: return "listen-only";
    case qusbcanb::CanMode::SelfTest: return "self-test";
    }
    return "unknown";
}

qusbcanb::CanFrame makeFrame(std::uint32_t baseId, std::size_t seq)
{
    qusbcanb::CanFrame frame;
    frame.id = baseId + static_cast<std::uint32_t>(seq % 0x100u);
    frame.dlc = 8;
    const std::uint32_t s = static_cast<std::uint32_t>(seq);
    frame.data[0] = static_cast<std::uint8_t>(s & 0xffu);
    frame.data[1] = static_cast<std::uint8_t>((s >> 8) & 0xffu);
    frame.data[2] = static_cast<std::uint8_t>((s >> 16) & 0xffu);
    frame.data[3] = static_cast<std::uint8_t>((s >> 24) & 0xffu);
    frame.data[4] = 'Q';
    frame.data[5] = 'U';
    frame.data[6] = 'S';
    frame.data[7] = 'B';
    return frame;
}

std::vector<qusbcanb::CanFrame> makeBatch(std::uint32_t baseId, std::size_t begin, std::size_t count)
{
    std::vector<qusbcanb::CanFrame> frames;
    frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        frames.push_back(makeFrame(baseId, begin + i));
    return frames;
}

void printEndpoints(qusbcanb::Channel channel)
{
    const auto ep = qusbcanb::LowLevelDevice::endpoints(channel);
    std::cout << qusbcanb::channelName(channel)
              << ": CMD OUT 0x" << std::hex << int(ep.command_out)
              << " CMD IN 0x" << int(ep.command_in)
              << " MSG OUT 0x" << int(ep.message_out)
              << " MSG IN 0x" << int(ep.message_in) << std::dec << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    try {
        opt = parseOptions(argc, argv);
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cout << "QUsbCanB clean POSIX regression: count=" << opt.count
              << " per channel, bitrate=" << opt.bitrate
              << ", mode=" << modeName(opt.mode)
              << ", batch=" << opt.batch << '\n';
    printEndpoints(qusbcanb::Channel::Can1);
    printEndpoints(qusbcanb::Channel::Can2);

    qusbcanb::LowLevelDevice dev;
    if (!dev.open()) {
        std::cerr << "open failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    if (!dev.configureBoth(opt.bitrate, opt.mode)) {
        std::cerr << "configure failed: " << dev.lastError() << '\n';
        return EXIT_FAILURE;
    }

    std::size_t tx1 = 0;
    std::size_t tx2 = 0;
    std::size_t rx1 = 0;
    std::size_t rx2 = 0;
    std::size_t err1 = 0;
    std::size_t err2 = 0;

    const auto started = std::chrono::steady_clock::now();
    auto lastProgress = started;

    while (tx1 < opt.count || tx2 < opt.count || rx1 < opt.count || rx2 < opt.count) {
        if (tx1 < opt.count) {
            const std::size_t n = std::min(opt.batch, opt.count - tx1);
            const auto frames = makeBatch(0x100, tx1, n);
            if (!dev.send(qusbcanb::Channel::Can1, frames)) {
                ++err1;
                std::cerr << "send CAN1 failed: " << dev.lastError() << '\n';
                break;
            }
            tx1 += n;
        }

        if (tx2 < opt.count) {
            const std::size_t n = std::min(opt.batch, opt.count - tx2);
            const auto frames = makeBatch(0x200, tx2, n);
            if (!dev.send(qusbcanb::Channel::Can2, frames)) {
                ++err2;
                std::cerr << "send CAN2 failed: " << dev.lastError() << '\n';
                break;
            }
            tx2 += n;
        }

        std::vector<qusbcanb::CanFrame> frames;
        if (dev.receive(qusbcanb::Channel::Can1, frames, 256, std::chrono::milliseconds{1}))
            rx1 += frames.size();
        else
            ++err1;

        if (dev.receive(qusbcanb::Channel::Can2, frames, 256, std::chrono::milliseconds{1}))
            rx2 += frames.size();
        else
            ++err2;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastProgress >= std::chrono::seconds{1}) {
            std::cout << "progress: TX1=" << tx1 << " TX2=" << tx2
                      << " RX1=" << rx1 << " RX2=" << rx2
                      << " err1=" << err1 << " err2=" << err2 << '\n';
            lastProgress = now;
        }

        if (tx1 >= opt.count && tx2 >= opt.count && (rx1 < opt.count || rx2 < opt.count)) {
            if (now - started > std::chrono::seconds{30})
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    (void)dev.stop(qusbcanb::Channel::Can1);
    (void)dev.stop(qusbcanb::Channel::Can2);

    std::cout << "summary: produced1=" << opt.count
              << " produced2=" << opt.count
              << " TX1=" << tx1 << '/' << opt.count
              << " TX2=" << tx2 << '/' << opt.count
              << " RX1=" << rx1
              << " RX2=" << rx2
              << " err1=" << err1
              << " err2=" << err2 << '\n';

    return (tx1 == opt.count && tx2 == opt.count && rx1 >= opt.count && rx2 >= opt.count && err1 == 0 && err2 == 0)
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
