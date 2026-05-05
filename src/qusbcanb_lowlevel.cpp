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
    close();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_lastError.clear();

    int rc = libusb_init(&m_context);
    if (rc != LIBUSB_SUCCESS) {
        setError("libusb_init", rc);
        return false;
    }

    m_handle = libusb_open_device_with_vid_pid(m_context, config.vendor_id, config.product_id);
    if (!m_handle) {
        std::ostringstream os;
        os << "USB device not found: vid=0x" << std::hex << config.vendor_id
           << " pid=0x" << config.product_id;
        setError(os.str());
        libusb_exit(std::exchange(m_context, nullptr));
        return false;
    }

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
        (void)libusb_detach_kernel_driver(m_handle, config.interface_number);
        rc = libusb_claim_interface(m_handle, config.interface_number);
    }

    if (rc != LIBUSB_SUCCESS) {
        if (rc == LIBUSB_ERROR_BUSY) {
            setError("claim interface failed: LIBUSB_ERROR_BUSY. Stop other users of the adapter "
                     "or unplug/replug it.");
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

bool LowLevelDevice::init(Channel channel, const BitTiming &timing, CanMode mode)
{
    std::array<std::uint8_t, UsbPacketSize> packet{};
    writeLe32(packet.data() + 0, CommandInit);
    writeLe32(packet.data() + 4, 0x00000001u);       // acceptance code
    writeLe32(packet.data() + 8, 0xFFFFFFFFu);       // acceptance mask: receive all
    writeLe32(packet.data() + 12, 0x00000000u);
    writeLe32(packet.data() + 16, 0x00000001u);      // single filter
    writeLe32(packet.data() + 20, 0x00000000u);
    writeLe32(packet.data() + 24, timing.timing0);
    writeLe32(packet.data() + 28, timing.timing1);
    writeLe32(packet.data() + 32, static_cast<std::uint32_t>(mode));
    writeLe32(packet.data() + 36, 0x00000001u);      // required by observed protocol
    return sendCommandPacket(channel, packet);
}

bool LowLevelDevice::start(Channel channel)
{
    return sendCommand(channel, CommandStart);
}

bool LowLevelDevice::stop(Channel channel)
{
    return sendCommand(channel, CommandStop);
}

bool LowLevelDevice::clearRx(Channel channel)
{
    const bool ok = sendCommand(channel, CommandClearRxBuffer);
    clearRxCache(channel);
    return ok;
}

bool LowLevelDevice::configure(Channel channel, std::uint32_t bitrate, CanMode mode)
{
    const BitTiming timing = bitTimingForBitrate(bitrate);

    // Stop is intentionally best-effort. Some clones report errors if a channel
    // was not started yet, but still accept INIT/START afterwards.
    (void)stop(channel);
    clearRxCache(channel);

    if (!init(channel, timing, mode))
        return false;
    if (!start(channel))
        return false;
    return clearRx(channel);
}

bool LowLevelDevice::configureBoth(std::uint32_t bitrate, CanMode mode)
{
    return configure(Channel::Can1, bitrate, mode) &&
           configure(Channel::Can2, bitrate, mode);
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

        if (!writePacket(messageEndpointOut(channel), packet, m_config.io_timeout_ms))
            return false;
        offset += count;
    }
    return true;
}

bool LowLevelDevice::receive(Channel channel,
                             std::vector<CanFrame> &frames,
                             std::size_t maxFrames,
                             std::chrono::milliseconds timeout)
{
    frames.clear();
    if (maxFrames == 0)
        return true;

    drainRxCache(channel, frames, maxFrames);
    if (frames.size() >= maxFrames)
        return true;

    DeviceStatus st{};
    if (!status(channel, st))
        return false;
    if (st.rx_pending == 0)
        return true;

    const std::uint32_t buffersToRead = std::min<std::uint32_t>((st.rx_pending + 2u) / 3u + 1u, 64u);
    const unsigned int timeoutMs = static_cast<unsigned int>(std::max<std::int64_t>(1, timeout.count()));

    std::vector<CanFrame> decoded;
    decoded.reserve(static_cast<std::size_t>(buffersToRead) * MaxFramesPerUsbPacket);

    for (std::uint32_t b = 0; b < buffersToRead; ++b) {
        std::array<std::uint8_t, UsbPacketSize> packet{};
        if (!readPacket(messageEndpointIn(channel), packet, timeoutMs)) {
            // Keep already decoded frames; a final short timeout can happen after
            // the hardware has already delivered the pending messages.
            if (!decoded.empty())
                break;
            return false;
        }

        const std::uint8_t count = std::min<std::uint8_t>(packet[0], MaxFramesPerUsbPacket);
        for (std::uint8_t i = 0; i < count; ++i) {
            CanFrame frame;
            if (decodeFrame(packet.data() + 1 + i * CanObjectSize, frame))
                decoded.push_back(frame);
        }

        if (decoded.size() >= st.rx_pending)
            break;
    }

    if (!decoded.empty()) {
        std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
        auto &queue = m_rxCache[channelIndex(channel)];
        queue.insert(queue.end(), decoded.begin(), decoded.end());
    }

    drainRxCache(channel, frames, maxFrames);
    return true;
}

bool LowLevelDevice::status(Channel channel, DeviceStatus &statusOut)
{
    statusOut = DeviceStatus{};

    std::array<std::uint8_t, UsbPacketSize> packet{};
    writeLe32(packet.data(), CommandMessageStatus);
    if (!sendCommandPacket(channel, packet))
        return false;

    std::array<std::uint8_t, UsbPacketSize> response{};
    if (!readPacket(commandEndpointIn(channel), response, m_config.io_timeout_ms))
        return false;

    statusOut.rx_pending = readLe32(response.data() + 4);
    statusOut.tx_pending = static_cast<std::uint32_t>(response[8]) |
                           (static_cast<std::uint32_t>(response[9]) << 8);
    return true;
}

ChannelEndpoints LowLevelDevice::endpoints(Channel channel, const DeviceConfig &config) noexcept
{
    return channel == Channel::Can1 ? config.can1 : config.can2;
}

bool LowLevelDevice::writePacket(std::uint8_t endpoint,
                                 const std::array<std::uint8_t, UsbPacketSize> &packet,
                                 unsigned int timeout_ms)
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
        os << "short USB write on endpoint 0x" << std::hex << int(endpoint)
           << ": transferred " << std::dec << transferred << " of " << packet.size();
        setError(os.str());
        return false;
    }
    return true;
}

bool LowLevelDevice::readPacket(std::uint8_t endpoint,
                                std::array<std::uint8_t, UsbPacketSize> &packet,
                                unsigned int timeout_ms)
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
    return writePacket(commandEndpointOut(channel), packet, m_config.io_timeout_ms);
}

bool LowLevelDevice::sendCommand(Channel channel, std::uint32_t command)
{
    std::array<std::uint8_t, UsbPacketSize> packet{};
    writeLe32(packet.data(), command);
    return sendCommandPacket(channel, packet);
}

std::uint8_t LowLevelDevice::commandEndpointOut(Channel channel) const noexcept
{
    return endpoints(channel, m_config).command_out;
}

std::uint8_t LowLevelDevice::commandEndpointIn(Channel channel) const noexcept
{
    return endpoints(channel, m_config).command_in;
}

std::uint8_t LowLevelDevice::messageEndpointOut(Channel channel) const noexcept
{
    return endpoints(channel, m_config).message_out;
}

std::uint8_t LowLevelDevice::messageEndpointIn(Channel channel) const noexcept
{
    return endpoints(channel, m_config).message_in;
}

void LowLevelDevice::clearRxCache(Channel channel)
{
    std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
    m_rxCache[channelIndex(channel)].clear();
}

std::size_t LowLevelDevice::drainRxCache(Channel channel, std::vector<CanFrame> &frames, std::size_t maxFrames)
{
    std::lock_guard<std::mutex> cacheLock(m_rxCacheMutex);
    auto &queue = m_rxCache[channelIndex(channel)];
    const std::size_t room = maxFrames > frames.size() ? maxFrames - frames.size() : 0;
    const std::size_t take = std::min(room, queue.size());
    if (take > 0) {
        frames.insert(frames.end(), queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(take));
        queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return take;
}

void LowLevelDevice::setError(const std::string &where, int libusb_error)
{
    m_lastError = where + ": " + libusb_error_name(libusb_error);
}

void LowLevelDevice::setError(std::string message)
{
    m_lastError = std::move(message);
}

std::size_t LowLevelDevice::channelIndex(Channel channel) noexcept
{
    return channel == Channel::Can1 ? 0u : 1u;
}

void LowLevelDevice::encodeFrame(const CanFrame &frame, std::uint8_t *dst21)
{
    std::memset(dst21, 0, CanObjectSize);
    writeLe32(dst21 + 0, frame.id & (frame.extended ? 0x1fffffffu : 0x7ffu));
    writeLe32(dst21 + 4, 0);                         // timestamp: ignored for TX
    dst21[8] = 1;                                    // time_flag
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

const char *channelName(Channel channel) noexcept
{
    return channel == Channel::Can1 ? "CAN1" : "CAN2";
}

} // namespace qusbcanb
