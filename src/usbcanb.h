#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libusb-1.0/libusb.h>

class UsbCanB
{
public:
    static constexpr uint16_t VendorId = 0x04d8;
    static constexpr uint16_t ProductId = 0x0053;
    static constexpr int InterfaceNumber = 0;
    static constexpr int UsbPacketSize = 64;

    enum class Mode : uint8_t {
        Normal = 0,
        ListenOnly = 1,
        SelfTest = 2
    };

    struct Frame {
        uint32_t id = 0;
        bool extended = false;
        bool remote = false;
        uint8_t dlc = 0;
        std::array<uint8_t, 8> data{};
    };

    struct Channel {
        int index = 1;
        uint8_t cmdOutEndpoint = 0x02;  // Clear/Init/Start/Reset endpoint from libcontrolcan
        uint8_t txOutEndpoint = 0x01;   // CAN frame TX endpoint from libcontrolcan
        uint8_t rxInEndpoint = 0x81;    // CAN frame RX endpoint from libcontrolcan
    };

    UsbCanB();
    ~UsbCanB();

    UsbCanB(const UsbCanB&) = delete;
    UsbCanB& operator=(const UsbCanB&) = delete;

    bool open();
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    bool handshake(int timeoutMs = 1000);
    bool clearBuffer(const Channel& ch, int timeoutMs = 1000);
    bool initCan125k(const Channel& ch, Mode mode = Mode::Normal, int timeoutMs = 1000);
    bool initCan125kCompact(const Channel& ch, Mode mode = Mode::Normal, int timeoutMs = 1000);
    bool startCan(const Channel& ch, int timeoutMs = 1000);
    bool resetCan(const Channel& ch, int timeoutMs = 1000);

    int transmit(const Channel& ch, const std::vector<Frame>& frames, int timeoutMs = 1000);
    std::vector<Frame> receive(const Channel& ch, int timeoutMs = 20);

    const std::string& lastError() const { return lastError_; }
    static Channel channel1();
    static Channel channel2();

private:
    enum Command : uint32_t {
        CmdInitCan = 0x01,
        CmdStartCan = 0x02,
        CmdResetCan = 0x03,
        CmdClearBuffer = 0x05
    };

    struct InitPayload {
        uint32_t accCode;
        uint32_t accMask;
        uint32_t reserved;
        uint8_t filter;
        uint8_t timing0;
        uint8_t timing1;
        uint8_t mode;
    };

    bool writePacket(uint8_t endpoint, const uint8_t* data, int len, int timeoutMs);
    int readPacket(uint8_t endpoint, uint8_t* data, int len, int timeoutMs, bool timeoutIsError);
    bool sendCommand(const Channel& ch, Command cmd, const uint8_t* payload = nullptr, int payloadLen = 0, int timeoutMs = 1000);
    void setUsbError(const char* what, uint8_t endpoint, int rc, int transferred, int requested);
    void clearHalt(uint8_t endpoint);

    static std::array<uint8_t, UsbPacketSize> makeCommandPacket(Command cmd, const uint8_t* payload, int payloadLen);
    static std::array<uint8_t, UsbPacketSize> makeInitConfigPacket(Mode mode);
    static std::array<uint8_t, UsbPacketSize> packTxPacket(const std::vector<Frame>& frames, int first, int count);
    static Frame unpackFrame21(const uint8_t* p);
    static void packFrame21(uint8_t* p, const Frame& frame);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool interfaceClaimed_ = false;
    std::string lastError_;
};
