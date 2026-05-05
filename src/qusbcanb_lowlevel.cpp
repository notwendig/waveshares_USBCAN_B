#include "qusbcanb_lowlevel.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

#include <libusb-1.0/libusb.h>

namespace qusbcanb {
namespace {
constexpr std::size_t UsbPacketSize = 64;
constexpr std::size_t CanObjectSize = 21;
constexpr std::size_t MaxFramesPerUsbPacket = 3;

constexpr std::uint32_t CommandInit = 0x01;
constexpr std::uint32_t CommandStart = 0x02;
constexpr std::uint32_t CommandStop = 0x03;
constexpr std::uint32_t CommandClearRxBuffer = 0x05;
constexpr std::uint32_t CommandMessageStatus = 0x0A;

constexpr std::uint8_t SendTypeNoRetry = 0x01;
constexpr std::uint8_t SendTypeEcho = 0x02;

std::uint8_t channelNumber(Channel channel)
{
    return channel == Channel::Can1 ? 0u : 1u;
}

std::uint32_t readLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeLe32(std::uint8_t *p, std::uint32_t value)
{
    p[0] = static_cast<std::uint8_t>(value & 0xffu);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
}
} // namespace

LowLevelDevice::LowLevelDevice() = default;

LowLevelDevice::~LowLevelDevice()
{
    close();
}

LowLevelDevice::LowLevelDevice(LowLevelDevice &&other) noexcept
{
    std::scoped_lock lock(other.m_mutex, other.m_rxCacheMutex);
    m_context = std::exchange(other.m_context, nullptr);
    m_handle = std::exchange(other.m_handle, nullptr);
    m_config = other.m_config;
    m_rxCache = std::move(other.m_rxCache);
    m_lastError = std::move(other.m_lastError);
}

LowLevelDevice &LowLevelDevice::operator=(LowLevelDevice &&other) noexcept
{
    if (this == &other)
        return *this;

    close();
    std::scoped_lock lock(m_mutex, m_rxCacheMutex, other.m_mutex, other.m_rxCacheMutex);
    m_context = std::exchange(other.m_context, nullptr);
    m_handle = std::exchange(other.m_handle, nullptr);
    m_config = other.m_config;
    m_rxCache = std::move(other.m_rxCache);
    m_lastError = std::move(other.m_lastError);
    return *this;
}

bool LowLevelDevice::open(const DeviceConfig &config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    close();
    m_config = config;
    m_lastError.clear();

    int rc = libusb_init(&m_context);
    if (rc != LIBUSB_SUCCESS) {
        setError("libusb_init", rc);
        return false;
    }

    m_handle = libusb_open_device_with_vid_pid(m_context, config.vendor_id, config.product_id);
    if (!m_handle) {
        setError("USB device not found: vid=0x" + std::to_string(config.vendor_id) +
                 " pid=0x" + std::to_string(config.product_id));
        libusb_exit(std::exchange(m_context, nullptr));
        return false;
    }

    // Let libusb detach kernel drivers automatically where the platform supports it.
    // This is harmless when no kernel driver is attached and avoids many
    // LIBUSB_ERROR_BUSY cases on Linux.
    if (config.detach_kernel_driver)
        (void)libusb_set_auto_detach_kernel_driver(m_handle, 1);

    rc = libusb_set_configuration(m_handle, config.usb_config);
    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_BUSY) {
        setError("libusb_set_configuration", rc);
        close();
        return false;
    }

    if (config.detach_kernel_driver) {
        const int active = libusb_kernel_driver_active(m_handle, config.interface_number);
        if (active == 1)
            (void)libusb_detach_kernel_driver(m_handle, config.interface_number);
    }

    rc = libusb_claim_interface(m_handle, config.interface_number);
    if (rc == LIBUSB_ERROR_BUSY && config.detach_kernel_driver) {
        // One more explicit detach+claim pass helps with some CANalyst-II clones.
        (void)libusb_detach_kernel_driver(m_handle, config.interface_number);
        rc = libusb_claim_interface(m_handle, config.interface_number);
    }
    if (rc != LIBUSB_SUCCESS) {
        if (rc == LIBUSB_ERROR_BUSY) {
            setError("claim interface 0 failed: LIBUSB_ERROR_BUSY. "
                     "The USB interface is already claimed. Stop other users of the device "
                     "first, for example old test programs, SavvyCAN/canalystii-bridge, "
                     "python-can processes, or unplug/replug the adapter.");
        } else {
            setError("libusb_claim_interface", rc);
        }
        close();
        return false;
    }

    return true;
}

void LowLevelDevice::close() noexcept
{
    {
        std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
        for (auto &queue : m_rxCache)
            queue.clear();
    }

    if (m_handle) {
        libusb_release_interface(m_handle, m_config.interface_number);
        libusb_close(m_handle);
        m_handle = nullptr;
    }
    if (m_context) {
        libusb_exit(m_context);
        m_context = nullptr;
    }
}

bool LowLevelDevice::isOpen() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_handle != nullptr;
}

bool LowLevelDevice::open(void *errorOut)
{
    (void)errorOut;
    return open(DeviceConfig{});
}

bool LowLevelDevice::reset(Channel channel)
{
    // There is no known dedicated reset opcode in the CANalyst-II USB protocol.
    // For compatibility, stop the channel and clear its RX buffer.
    (void)sendSimpleCommand(channel, CommandStop);
    return sendSimpleCommand(channel, CommandClearRxBuffer);
}

bool LowLevelDevice::init(Channel channel, const BitTiming &timing)
{
    // CANalyst-II InitCommand, 16 little-endian 32-bit words:
    // command, acc_code, acc_mask, reserved0, filter, reserved1, timing0, timing1, mode, unknown2, padding...
    std::array<std::uint8_t, UsbPacketSize> packet{};
    writeLe32(packet.data() + 0, CommandInit);
    writeLe32(packet.data() + 4, 0x00000001u);       // acc_code: accept all with mask below
    writeLe32(packet.data() + 8, 0xFFFFFFFFu);       // acc_mask
    writeLe32(packet.data() + 12, 0x00000000u);
    writeLe32(packet.data() + 16, 0x00000001u);      // single-filter placeholder
    writeLe32(packet.data() + 20, 0x00000000u);
    writeLe32(packet.data() + 24, timing.timing0);
    writeLe32(packet.data() + 28, timing.timing1);
    writeLe32(packet.data() + 32, 0x00000000u);      // normal mode
    writeLe32(packet.data() + 36, 0x00000001u);      // required by observed protocol
    return sendCommandPacket(channel, packet);
}

bool LowLevelDevice::start(Channel channel)
{
    return sendSimpleCommand(channel, CommandStart);
}

bool LowLevelDevice::stop(Channel channel)
{
    return sendSimpleCommand(channel, CommandStop);
}

bool LowLevelDevice::send(Channel channel, const CanFrame &frame)
{
    return send(channel, std::vector<CanFrame>{frame});
}

bool LowLevelDevice::send(Channel channel, const std::vector<CanFrame> &frames)
{
    if (frames.empty())
        return true;

    std::size_t offset = 0;
    while (offset < frames.size()) {
        std::array<std::uint8_t, UsbPacketSize> packet{};
        const std::size_t count = std::min(MaxFramesPerUsbPacket, frames.size() - offset);
        packet[0] = static_cast<std::uint8_t>(count);

        for (std::size_t i = 0; i < count; ++i)
            encodeFrame(frames[offset + i], packet.data() + 1 + i * CanObjectSize);

        if (!writePacketToEndpoint(endpointTxOut(channel), packet, m_config.io_timeout_ms))
            return false;
        offset += count;
    }
    return true;
}

bool LowLevelDevice::receive(Channel channel, std::vector<CanFrame> &frames, std::chrono::milliseconds timeout)
{
    frames.clear();

    std::uint32_t rxPending = 0;
    std::uint32_t txPending = 0;
    if (!readMessageStatus(channel, rxPending, txPending))
        return false;
    if (rxPending == 0)
        return true;

    // The hardware returns MessageBuffer packets: 1 count byte + 3 x 21-byte messages.
    // Read one extra packet like the known Python driver to avoid occasional USB packet
    // fragmentation leaving the last messages for a later poll.
    const std::uint32_t buffersToRead = std::min<std::uint32_t>((rxPending + 2u) / 3u + 1u, 64u);
    const unsigned int timeoutMs = static_cast<unsigned int>(std::max<std::int64_t>(1, timeout.count()));

    for (std::uint32_t b = 0; b < buffersToRead; ++b) {
        std::array<std::uint8_t, UsbPacketSize> packet{};
        if (!readPacketFromEndpoint(endpointRxIn(channel), packet, timeoutMs)) {
            // If we already decoded something, return it instead of turning a trailing
            // empty/timeout read into a hard failure.
            return !frames.empty();
        }

        const std::uint8_t count = std::min<std::uint8_t>(packet[0], MaxFramesPerUsbPacket);
        for (std::uint8_t i = 0; i < count; ++i) {
            CanFrame frame;
            if (decodeFrame(packet.data() + 1 + i * CanObjectSize, frame))
                frames.push_back(frame);
        }

        if (frames.size() >= rxPending)
            break;
    }
    return true;
}

bool LowLevelDevice::writePacket(Channel channel, const std::array<std::uint8_t, UsbPacketSize> &packet, unsigned int timeout_ms)
{
    return writePacketToEndpoint(endpointCommandOut(channel), packet, timeout_ms);
}

bool LowLevelDevice::writePacketToEndpoint(unsigned char endpoint, const std::array<std::uint8_t, UsbPacketSize> &packet, unsigned int timeout_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_handle) {
        setError("USB device is not open");
        return false;
    }

    int transferred = 0;
    const int rc = libusb_bulk_transfer(m_handle,
                                        endpoint,
                                        const_cast<unsigned char *>(packet.data()),
                                        static_cast<int>(packet.size()),
                                        &transferred,
                                        timeout_ms);
    if (rc != LIBUSB_SUCCESS) {
        std::ostringstream os;
        os << "libusb_bulk_transfer OUT endpoint 0x" << std::hex << int(endpoint);
        setError(os.str(), rc);
        return false;
    }
    if (transferred != static_cast<int>(packet.size())) {
        std::ostringstream os;
        os << "short USB write on endpoint 0x" << std::hex << int(endpoint);
        setError(os.str());
        return false;
    }
    return true;
}

bool LowLevelDevice::readPacket(Channel channel, std::array<std::uint8_t, UsbPacketSize> &packet, unsigned int timeout_ms)
{
    return readPacketFromEndpoint(endpointRxIn(channel), packet, timeout_ms);
}

bool LowLevelDevice::readPacketFromEndpoint(unsigned char endpoint, std::array<std::uint8_t, UsbPacketSize> &packet, unsigned int timeout_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_handle) {
        setError("USB device is not open");
        return false;
    }

    int transferred = 0;
    const int rc = libusb_bulk_transfer(m_handle,
                                        endpoint,
                                        packet.data(),
                                        static_cast<int>(packet.size()),
                                        &transferred,
                                        timeout_ms);
    if (rc != LIBUSB_SUCCESS) {
        std::ostringstream os;
        os << "libusb_bulk_transfer IN endpoint 0x" << std::hex << int(endpoint);
        setError(os.str(), rc);
        return false;
    }
    if (transferred <= 0) {
        std::ostringstream os;
        os << "empty USB read on endpoint 0x" << std::hex << int(endpoint);
        setError(os.str());
        return false;
    }
    return true;
}

bool LowLevelDevice::sendCommandPacket(Channel channel, const std::array<std::uint8_t, UsbPacketSize> &packet)
{
    return writePacketToEndpoint(endpointCommandOut(channel), packet, m_config.io_timeout_ms);
}

bool LowLevelDevice::sendSimpleCommand(Channel channel, std::uint32_t command)
{
    std::array<std::uint8_t, UsbPacketSize> packet{};
    writeLe32(packet.data(), command);
    return sendCommandPacket(channel, packet);
}

bool LowLevelDevice::readMessageStatus(Channel channel, std::uint32_t &rxPending, std::uint32_t &txPending)
{
    rxPending = 0;
    txPending = 0;

    std::array<std::uint8_t, UsbPacketSize> packet{};
    writeLe32(packet.data(), CommandMessageStatus);
    if (!sendCommandPacket(channel, packet))
        return false;

    std::array<std::uint8_t, UsbPacketSize> response{};
    if (!readPacketFromEndpoint(endpointCommandIn(channel), response, m_config.io_timeout_ms))
        return false;

    rxPending = readLe32(response.data() + 4);
    txPending = static_cast<std::uint32_t>(response[8]) | (static_cast<std::uint32_t>(response[9]) << 8);
    return true;
}

unsigned char LowLevelDevice::endpointCommandOut(Channel channel) const noexcept
{
    return channel == Channel::Can1 ? m_config.command_out_can1 : m_config.command_out_can2;
}

unsigned char LowLevelDevice::endpointCommandIn(Channel channel) const noexcept
{
    return static_cast<unsigned char>(endpointCommandOut(channel) | 0x80u);
}

unsigned char LowLevelDevice::endpointTxOut(Channel channel) const noexcept
{
    return channel == Channel::Can1 ? m_config.tx_out_can1 : m_config.tx_out_can2;
}

unsigned char LowLevelDevice::endpointRxIn(Channel channel) const noexcept
{
    return channel == Channel::Can1 ? m_config.rx_in_can1 : m_config.rx_in_can2;
}

unsigned char LowLevelDevice::endpointOut(Channel channel) const noexcept
{
    return endpointTxOut(channel);
}

unsigned char LowLevelDevice::endpointIn(Channel channel) const noexcept
{
    return endpointRxIn(channel);
}

void LowLevelDevice::setError(const std::string &where, int libusb_error)
{
    m_lastError = where + ": " + libusb_error_name(libusb_error);
}

void LowLevelDevice::setError(std::string message)
{
    m_lastError = std::move(message);
}


Channel LowLevelDevice::channelFromInt(int channel) noexcept
{
    // The Qt wrapper uses 0 = both, 1 = CAN1, 2 = CAN2.
    // For a single-channel call, 0 and 1 therefore map to CAN1.
    return channel <= 1 ? Channel::Can1 : Channel::Can2;
}

ChannelEndpoints LowLevelDevice::endpointsForChannel(int channel, const DeviceConfig &config)
{
    const bool can2 = channel > 1;
    const unsigned char commandOut = can2 ? config.command_out_can2 : config.command_out_can1;
    const unsigned char txOut = can2 ? config.tx_out_can2 : config.tx_out_can1;
    const unsigned char rxIn = can2 ? config.rx_in_can2 : config.rx_in_can1;
    const unsigned char commandIn = static_cast<unsigned char>(commandOut | 0x80u);

    ChannelEndpoints ep{};
    ep.bulk_out = txOut;
    ep.bulk_in = rxIn;
    ep.out = txOut;
    ep.in = rxIn;
    ep.tx = txOut;
    ep.rx = rxIn;
    ep.bulkOut = txOut;
    ep.bulkIn = rxIn;
    ep.outEndpoint = txOut;
    ep.inEndpoint = rxIn;
    ep.endpointOut = txOut;
    ep.endpointIn = rxIn;
    ep.commandOut = commandOut;
    ep.commandIn = commandIn;
    ep.txOut = txOut;
    ep.rxIn = rxIn;
    ep.command_out = commandOut;
    ep.command_in = commandIn;
    ep.tx_out = txOut;
    ep.rx_in = rxIn;
    return ep;
}

bool LowLevelDevice::configureAndStart(int channel, std::uint32_t bitrate, std::uint8_t mode, void *errorOut)
{
    (void)mode;
    (void)errorOut;

    const Channel ch = channelFromInt(channel);
    const BitTiming timing = bitTimingForBitrate(bitrate);

    // Use the observed CANalyst-II sequence: INIT then START.
    // A fake reset command corrupts the firmware state on some clones.
    if (!init(ch, timing))
        return false;
    if (!start(ch))
        return false;
    (void)sendSimpleCommand(ch, CommandClearRxBuffer);
    {
        std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
        m_rxCache[channelNumber(ch)].clear();
    }
    return true;
}

bool LowLevelDevice::configureBothAndStart(std::uint32_t bitrate, std::uint8_t mode, void *errorOut)
{
    (void)mode;
    (void)errorOut;

    const BitTiming timing = bitTimingForBitrate(bitrate);
    const Channel channels[] = {Channel::Can1, Channel::Can2};
    for (Channel ch : channels) {
        if (!init(ch, timing))
            return false;
        if (!start(ch))
            return false;
        (void)sendSimpleCommand(ch, CommandClearRxBuffer);
        {
            std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
            m_rxCache[channelNumber(ch)].clear();
        }
    }
    return true;
}

int LowLevelDevice::writeFrames(int channel, const std::vector<CanFrame> &frames, void *errorOut)
{
    (void)errorOut;
    if (frames.empty())
        return 0;
    if (!send(channelFromInt(channel), frames))
        return -1;
    return static_cast<int>(frames.size());
}

FrameList LowLevelDevice::readFrames(int channel, int maxFrames, void *errorOut)
{
    (void)errorOut;
    FrameList result;
    if (maxFrames <= 0)
        return result;

    const Channel ch = channelFromInt(channel);
    const std::size_t cacheIndex = channelNumber(ch);
    const std::size_t wanted = static_cast<std::size_t>(maxFrames);

    auto drainCache = [&]() {
        std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
        auto &queue = m_rxCache[cacheIndex];
        const std::size_t take = std::min<std::size_t>(wanted - result.size(), queue.size());
        if (take > 0) {
            result.insert(result.end(), queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(take));
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(take));
        }
    };

    // First return frames that were decoded during an earlier poll but could not
    // be returned because older Qt code often calls readFrames(channel, 1).
    // Without this cache, one USB packet with 3 CAN messages returned only one
    // frame and silently discarded the other two. That produced RX~=count/3.
    drainCache();
    if (result.size() >= wanted)
        return result;

    std::vector<CanFrame> received;
    if (!receive(ch, received, std::chrono::milliseconds{1}))
        return result;

    if (!received.empty()) {
        std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
        auto &queue = m_rxCache[cacheIndex];
        queue.insert(queue.end(), received.begin(), received.end());
    }

    drainCache();
    return result;
}

void LowLevelDevice::encodeFrame(const CanFrame &frame, std::uint8_t *dst21)
{
    std::memset(dst21, 0, CanObjectSize);
    writeLe32(dst21 + 0, frame.id & (frame.extended ? 0x1fffffffu : 0x7ffu));
    writeLe32(dst21 + 4, 0);                         // timestamp: ignored for TX
    dst21[8] = 1;                                    // time_flag, matches vendor/python-can usage
    dst21[9] = 0;                                    // send_type: normal retry, no local echo
    dst21[10] = frame.remote ? 1u : 0u;
    dst21[11] = frame.extended ? 1u : 0u;
    dst21[12] = std::min<std::uint8_t>(frame.dlc, 8);
    std::copy_n(frame.data.begin(), 8, dst21 + 13);
}

bool LowLevelDevice::decodeFrame(const std::uint8_t *src21, CanFrame &frame)
{
    frame.id = readLe32(src21 + 0);
    frame.remote = src21[10] != 0;
    frame.extended = src21[11] != 0;
    frame.dlc = std::min<std::uint8_t>(src21[12], 8);
    std::copy_n(src21 + 13, 8, frame.data.begin());
    return frame.dlc <= 8;
}

BitTiming bitTimingForBitrate(std::uint32_t bitrate)
{
    switch (bitrate) {
    case 1000000: return BitTiming{bitrate, 0x00, 0x14};
    case 800000:  return BitTiming{bitrate, 0x00, 0x16};
    case 500000:  return BitTiming{bitrate, 0x00, 0x1C};
    case 250000:  return BitTiming{bitrate, 0x01, 0x1C};
    case 125000:  return BitTiming{bitrate, 0x03, 0x1C};
    case 100000:  return BitTiming{bitrate, 0x04, 0x1C};
    case 50000:   return BitTiming{bitrate, 0x09, 0x1C};
    case 20000:   return BitTiming{bitrate, 0x18, 0x1C};
    case 10000:   return BitTiming{bitrate, 0x31, 0x1C};
    default:      return BitTiming{bitrate, 0x03, 0x1C};
    }
}

std::string frameToCandumpString(const CanFrame &frame)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    out << std::setw(frame.extended ? 8 : 3) << frame.id << '#';
    if (frame.remote) {
        out << 'R';
        return out.str();
    }
    for (std::uint8_t i = 0; i < std::min<std::uint8_t>(frame.dlc, 8); ++i)
        out << std::setw(2) << static_cast<unsigned>(frame.data[i]);
    return out.str();
}

} // namespace qusbcanb
