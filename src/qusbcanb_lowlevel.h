#pragma once

/*
 * QUsbCanB low-level POSIX/libusb backend
 *
 * This header is intentionally Qt-free.  The interface uses only C++ standard
 * library types and the qusbcanb namespace.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace qusbcanb {

struct CanFrame
{
    std::uint32_t id = 0;
    std::uint8_t dlc = 0;
    std::array<std::uint8_t, 8> data{};
    bool extended = false;
    bool remote = false;
};

enum class Channel : std::uint8_t
{
    Can1 = 0,
    Can2 = 1,
};

enum class CanMode : std::uint32_t
{
    Normal = 0,
    ListenOnly = 1,
    SelfTest = 2,
};

struct BitTiming
{
    std::uint32_t bitrate = 125000;
    std::uint8_t timing0 = 0x03;
    std::uint8_t timing1 = 0x1C;
};

struct ChannelEndpoints
{
    std::uint8_t command_out = 0;
    std::uint8_t command_in = 0;
    std::uint8_t message_out = 0;
    std::uint8_t message_in = 0;
};

struct DeviceStatus
{
    std::uint32_t rx_pending = 0;
    std::uint32_t tx_pending = 0;
};

struct DeviceConfig
{
    std::uint16_t vendor_id = 0x04d8;
    std::uint16_t product_id = 0x0053;
    int usb_config = 1;
    int interface_number = 0;

    // CANalyst-II / USBCANB endpoint layout:
    // CAN1: command OUT/IN 0x02/0x82, CAN message OUT/IN 0x01/0x81
    // CAN2: command OUT/IN 0x04/0x84, CAN message OUT/IN 0x03/0x83
    ChannelEndpoints can1{0x02, 0x82, 0x01, 0x81};
    ChannelEndpoints can2{0x04, 0x84, 0x03, 0x83};

    unsigned int io_timeout_ms = 100;
    bool detach_kernel_driver = true;
};

class LowLevelDevice
{
public:
    LowLevelDevice();
    ~LowLevelDevice();

    LowLevelDevice(const LowLevelDevice &) = delete;
    LowLevelDevice &operator=(const LowLevelDevice &) = delete;

    LowLevelDevice(LowLevelDevice &&other) noexcept;
    LowLevelDevice &operator=(LowLevelDevice &&other) noexcept;

    bool open(const DeviceConfig &config = DeviceConfig{});
    void close() noexcept;
    bool isOpen() const noexcept;

    const DeviceConfig &config() const noexcept { return m_config; }
    const std::string &lastError() const noexcept { return m_lastError; }

    bool init(Channel channel,
              const BitTiming &timing = BitTiming{},
              CanMode mode = CanMode::Normal);
    bool start(Channel channel);
    bool stop(Channel channel);
    bool clearRx(Channel channel);

    bool configure(Channel channel,
                   std::uint32_t bitrate = 125000,
                   CanMode mode = CanMode::Normal);
    bool configureBoth(std::uint32_t bitrate = 125000,
                       CanMode mode = CanMode::Normal);

    bool send(Channel channel, const CanFrame &frame);
    bool send(Channel channel, const std::vector<CanFrame> &frames);

    bool receive(Channel channel,
                 std::vector<CanFrame> &frames,
                 std::size_t maxFrames = 64,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds{1});

    bool status(Channel channel, DeviceStatus &status);

    static ChannelEndpoints endpoints(Channel channel,
                                      const DeviceConfig &config = DeviceConfig{}) noexcept;

private:
    bool writePacket(std::uint8_t endpoint,
                     const std::array<std::uint8_t, 64> &packet,
                     unsigned int timeout_ms);
    bool readPacket(std::uint8_t endpoint,
                    std::array<std::uint8_t, 64> &packet,
                    unsigned int timeout_ms);
    bool sendCommandPacket(Channel channel, const std::array<std::uint8_t, 64> &packet);
    bool sendCommand(Channel channel, std::uint32_t command);

    std::uint8_t commandEndpointOut(Channel channel) const noexcept;
    std::uint8_t commandEndpointIn(Channel channel) const noexcept;
    std::uint8_t messageEndpointOut(Channel channel) const noexcept;
    std::uint8_t messageEndpointIn(Channel channel) const noexcept;

    void clearRxCache(Channel channel);
    std::size_t drainRxCache(Channel channel, std::vector<CanFrame> &frames, std::size_t maxFrames);

    void setError(const std::string &where, int libusb_error);
    void setError(std::string message);

    static std::size_t channelIndex(Channel channel) noexcept;
    static void encodeFrame(const CanFrame &frame, std::uint8_t *dst21);
    static bool decodeFrame(const std::uint8_t *src21, CanFrame &frame);

    mutable std::mutex m_mutex;
    mutable std::mutex m_rxCacheMutex;
    std::array<std::vector<CanFrame>, 2> m_rxCache{};
    libusb_context *m_context = nullptr;
    libusb_device_handle *m_handle = nullptr;
    DeviceConfig m_config{};
    std::string m_lastError;
};

BitTiming bitTimingForBitrate(std::uint32_t bitrate);
std::string frameToCandumpString(const CanFrame &frame);
const char *channelName(Channel channel) noexcept;

} // namespace qusbcanb
