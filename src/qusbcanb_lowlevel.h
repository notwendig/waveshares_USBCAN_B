#pragma once

/*
 * QUsbCanB low-level POSIX/libusb backend
 *
 * This file is intentionally Qt-free.  It may be used from Qt code, but it
 * does not include, inherit from, or depend on Qt classes.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
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

class FrameList : public std::vector<CanFrame>
{
public:
    using std::vector<CanFrame>::vector;

    bool isEmpty() const noexcept { return empty(); }

    template <typename QtList>
    QtList toQtList() const
    {
        QtList result;
        using QtFrame = typename QtList::value_type;

        for (const CanFrame &frame : *this) {
            QtFrame qtFrame;
            qtFrame.setFrameId(frame.id);
            qtFrame.setExtendedFrameFormat(frame.extended);
            qtFrame.setFrameType(frame.remote ? QtFrame::RemoteRequestFrame : QtFrame::DataFrame);

            auto payload = qtFrame.payload();
            payload.resize(static_cast<int>(frame.dlc));
            for (std::uint8_t i = 0; i < frame.dlc; ++i)
                payload[static_cast<int>(i)] = static_cast<char>(frame.data[i]);
            qtFrame.setPayload(payload);

            result.append(qtFrame);
        }

        return result;
    }

    template <typename QtList>
    operator QtList() const
    {
        return toQtList<QtList>();
    }
};

enum class Channel : std::uint8_t
{
    Can1 = 0,
    Can2 = 1,
};

struct BitTiming
{
    std::uint32_t bitrate = 125000;
    std::uint8_t timing0 = 0x03; // SJA1000-style 125 kbit/s default
    std::uint8_t timing1 = 0x1C;
};


struct ChannelEndpoints
{
    unsigned char bulk_out = 0;
    unsigned char bulk_in = 0;

    // Compatibility aliases for older examples/tools.
    // CANalyst-II uses separate command and CAN-message endpoints:
    //   CAN1 command OUT/IN 0x02/0x82, message OUT/IN 0x01/0x81
    //   CAN2 command OUT/IN 0x04/0x84, message OUT/IN 0x03/0x83
    unsigned char out = 0;
    unsigned char in = 0;
    unsigned char tx = 0;
    unsigned char rx = 0;
    unsigned char bulkOut = 0;
    unsigned char bulkIn = 0;
    unsigned char outEndpoint = 0;
    unsigned char inEndpoint = 0;
    unsigned char endpointOut = 0;
    unsigned char endpointIn = 0;
    unsigned char commandOut = 0;
    unsigned char commandIn = 0;
    unsigned char txOut = 0;
    unsigned char rxIn = 0;
    unsigned char command_out = 0;
    unsigned char command_in = 0;
    unsigned char tx_out = 0;
    unsigned char rx_in = 0;
};

struct DeviceConfig
{
    std::uint16_t vendor_id = 0x04d8;
    std::uint16_t product_id = 0x0053;
    int usb_config = 1;
    int interface_number = 0;

    // CANalyst-II / USBCANB endpoint map observed by the original
    // libcontrolcan-style implementation and the open CANalyst-II protocol:
    //   CAN1: command OUT/IN 0x02/0x82, CAN-message OUT/IN 0x01/0x81
    //   CAN2: command OUT/IN 0x04/0x84, CAN-message OUT/IN 0x03/0x83
    // Command endpoints and CAN-message endpoints are intentionally separate.
    unsigned char command_out_can1 = 0x02;
    unsigned char tx_out_can1 = 0x01;
    unsigned char rx_in_can1 = 0x81;
    unsigned char command_out_can2 = 0x04;
    unsigned char tx_out_can2 = 0x03;
    unsigned char rx_in_can2 = 0x83;

    // Compatibility names used by older helper code. They refer to CAN frame
    // data endpoints, not to command endpoints.
    unsigned char bulk_out_can1 = tx_out_can1;
    unsigned char bulk_in_can1 = rx_in_can1;
    unsigned char bulk_out_can2 = tx_out_can2;
    unsigned char bulk_in_can2 = rx_in_can2;

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

    // Compatibility overload for the former Qt-facing worker code.
    // Keeps this header Qt-free: a QString* can bind to void*, but no Qt type is referenced here.
    bool open(void *errorOut);
    void close() noexcept;
    bool isOpen() const noexcept;

    const std::string &lastError() const noexcept { return m_lastError; }

    bool reset(Channel channel);
    bool init(Channel channel, const BitTiming &timing = BitTiming{});
    bool start(Channel channel);
    bool stop(Channel channel);

    bool send(Channel channel, const CanFrame &frame);
    bool send(Channel channel, const std::vector<CanFrame> &frames);

    bool receive(Channel channel,
                 std::vector<CanFrame> &frames,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds{100});

    // Compatibility API expected by qusbcanb_worker.cpp from the Qt wrapper.
    bool configureAndStart(int channel, std::uint32_t bitrate, std::uint8_t mode, void *errorOut = nullptr);
    bool configureBothAndStart(std::uint32_t bitrate, std::uint8_t mode, void *errorOut = nullptr);
    int writeFrames(int channel, const std::vector<CanFrame> &frames, void *errorOut = nullptr);

    template <typename FrameContainer>
    int writeFrames(int channel, const FrameContainer &frames, void *errorOut = nullptr)
    {
        std::vector<CanFrame> tmp;
        for (const auto &frame : frames)
            tmp.push_back(toCanFrame(frame));
        return writeFrames(channel, tmp, errorOut);
    }

    FrameList readFrames(int channel, int maxFrames = 1, void *errorOut = nullptr);

private:
    bool writePacket(Channel channel, const std::array<std::uint8_t, 64> &packet, unsigned int timeout_ms);
    bool writePacketToEndpoint(unsigned char endpoint, const std::array<std::uint8_t, 64> &packet, unsigned int timeout_ms);
    bool readPacket(Channel channel, std::array<std::uint8_t, 64> &packet, unsigned int timeout_ms);
    bool readPacketFromEndpoint(unsigned char endpoint, std::array<std::uint8_t, 64> &packet, unsigned int timeout_ms);
    bool sendCommandPacket(Channel channel, const std::array<std::uint8_t, 64> &packet);
    bool sendSimpleCommand(Channel channel, std::uint32_t command);
    bool readMessageStatus(Channel channel, std::uint32_t &rxPending, std::uint32_t &txPending);

    unsigned char endpointCommandOut(Channel channel) const noexcept;
    unsigned char endpointCommandIn(Channel channel) const noexcept;
    unsigned char endpointTxOut(Channel channel) const noexcept;
    unsigned char endpointRxIn(Channel channel) const noexcept;
    unsigned char endpointOut(Channel channel) const noexcept;
    unsigned char endpointIn(Channel channel) const noexcept;
    void setError(const std::string &where, int libusb_error);
    void setError(std::string message);

    static Channel channelFromInt(int channel) noexcept;

public:
    static ChannelEndpoints endpointsForChannel(int channel, const DeviceConfig &config = DeviceConfig{});

private:
    static CanFrame toCanFrame(const CanFrame &frame) { return frame; }

    template <typename QtFrame>
    static CanFrame toCanFrame(const QtFrame &frame)
    {
        CanFrame result;
        result.id = static_cast<std::uint32_t>(frame.frameId());
        result.extended = frame.hasExtendedFrameFormat();
        result.remote = frame.frameType() == QtFrame::RemoteRequestFrame;

        const auto payload = frame.payload();
        const auto payloadSize = payload.size();
        const std::size_t len = payloadSize <= 0
            ? 0u
            : std::min<std::size_t>(static_cast<std::size_t>(payloadSize), result.data.size());

        result.dlc = static_cast<std::uint8_t>(len);
        for (std::size_t i = 0; i < len; ++i)
            result.data[i] = static_cast<std::uint8_t>(payload.at(static_cast<int>(i)));

        return result;
    }

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

} // namespace qusbcanb


// Backward-compatibility namespace for the original Qt wrapper/examples.
// This intentionally extends/creates a real namespace instead of a namespace
// alias, so it remains compatible with existing code that already declares
// namespace QUsbCanB { ... } for the Qt-facing classes.
namespace QUsbCanB {
using ::qusbcanb::BitTiming;
using ::qusbcanb::CanFrame;
using ::qusbcanb::Channel;
using ::qusbcanb::ChannelEndpoints;
using ::qusbcanb::DeviceConfig;
using ::qusbcanb::FrameList;
using ::qusbcanb::LowLevelDevice;
using ::qusbcanb::bitTimingForBitrate;
using ::qusbcanb::frameToCandumpString;
} // namespace QUsbCanB

// Backward-compatibility aliases for old standalone example programs that used
// unqualified type names. They do not add Qt dependencies to the backend.
using LowLevelDevice = qusbcanb::LowLevelDevice;
using CanFrame = qusbcanb::CanFrame;
using FrameList = qusbcanb::FrameList;
using DeviceConfig = qusbcanb::DeviceConfig;
using BitTiming = qusbcanb::BitTiming;
using ChannelEndpoints = qusbcanb::ChannelEndpoints;
using qusbcanb::bitTimingForBitrate;
using qusbcanb::frameToCandumpString;
