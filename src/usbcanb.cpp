#include "usbcanb.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
static void putLe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xffu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

static uint32_t getLe32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
}

UsbCanB::UsbCanB() = default;

UsbCanB::~UsbCanB()
{
    close();
}

UsbCanB::Channel UsbCanB::channel1()
{
    return Channel{1, 0x02, 0x01, 0x81};
}

UsbCanB::Channel UsbCanB::channel2()
{
    return Channel{2, 0x04, 0x03, 0x83};
}

bool UsbCanB::open()
{
    close();
    lastError_.clear();

    int rc = libusb_init(&ctx_);
    if (rc != LIBUSB_SUCCESS) {
        lastError_ = std::string("libusb_init failed: ") + libusb_error_name(rc);
        return false;
    }

    handle_ = libusb_open_device_with_vid_pid(ctx_, VendorId, ProductId);
    if (!handle_) {
        lastError_ = "USB-CAN 04d8:0053 not found or permission denied";
        close();
        return false;
    }

    libusb_set_auto_detach_kernel_driver(handle_, 1);

    rc = libusb_set_configuration(handle_, 1);
    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_BUSY) {
        lastError_ = std::string("libusb_set_configuration(1) failed: ") + libusb_error_name(rc);
        close();
        return false;
    }

    rc = libusb_claim_interface(handle_, InterfaceNumber);
    if (rc != LIBUSB_SUCCESS) {
        lastError_ = std::string("libusb_claim_interface(0) failed: ") + libusb_error_name(rc);
        close();
        return false;
    }
    interfaceClaimed_ = true;

    return true;
}

void UsbCanB::close()
{
    if (handle_) {
        if (interfaceClaimed_) {
            libusb_release_interface(handle_, InterfaceNumber);
            interfaceClaimed_ = false;
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

bool UsbCanB::handshake(int timeoutMs)
{
    // Challenge bytes reconstructed from common CANalyst-II examples.
    // Some firmware versions ignore it; failure is intentionally non-fatal in test code.
    std::array<uint8_t, UsbPacketSize> challenge{};
    challenge[0] = 0xa5;
    challenge[1] = 0x5a;
    challenge[2] = 0x00;
    challenge[3] = 0x00;
    if (!writePacket(0x02, challenge.data(), UsbPacketSize, timeoutMs)) {
        return false;
    }
    writePacket(0x02, challenge.data(), UsbPacketSize, timeoutMs);

    std::array<uint8_t, UsbPacketSize> response{};
    readPacket(0x82, response.data(), UsbPacketSize, 20, false);
    return true;
}

bool UsbCanB::clearBuffer(const Channel& ch, int timeoutMs)
{
    return sendCommand(ch, CmdClearBuffer, nullptr, 0, timeoutMs);
}

bool UsbCanB::resetCan(const Channel& ch, int timeoutMs)
{
    return sendCommand(ch, CmdResetCan, nullptr, 0, timeoutMs);
}

bool UsbCanB::startCan(const Channel& ch, int timeoutMs)
{
    return sendCommand(ch, CmdStartCan, nullptr, 0, timeoutMs);
}

bool UsbCanB::initCan125k(const Channel& ch, Mode mode, int timeoutMs)
{
    // Exact 64-byte ControlCAN/CANalyst-II config layout from original usbcanb.h/libcontrolcan.
    // word0=CMD_INIT_CAN, word1=AccCode, word2=AccMask, word3=Reserved0,
    // word4=Filter, word5=Reserved1, word6=Timing0, word7=Timing1, word8=Mode.
    const auto packet = makeInitConfigPacket(mode);
    return writePacket(ch.cmdOutEndpoint, packet.data(), UsbPacketSize, timeoutMs);
}

bool UsbCanB::initCan125kCompact(const Channel& ch, Mode mode, int timeoutMs)
{
    // Diagnostic fallback: old compact 16-byte payload after command word.
    InitPayload p{};
    p.accCode = 0x00000000u;
    p.accMask = 0xffffffffu;
    p.reserved = 0x00000000u;
    p.filter = 1;
    p.timing0 = 0x03;
    p.timing1 = 0x1c;
    p.mode = static_cast<uint8_t>(mode);

    std::array<uint8_t, sizeof(InitPayload)> raw{};
    putLe32(raw.data() + 0, p.accCode);
    putLe32(raw.data() + 4, p.accMask);
    putLe32(raw.data() + 8, p.reserved);
    raw[12] = p.filter;
    raw[13] = p.timing0;
    raw[14] = p.timing1;
    raw[15] = p.mode;

    return sendCommand(ch, CmdInitCan, raw.data(), static_cast<int>(raw.size()), timeoutMs);
}

bool UsbCanB::sendCommand(const Channel& ch, Command cmd, const uint8_t* payload, int payloadLen, int timeoutMs)
{
    const auto packet = makeCommandPacket(cmd, payload, payloadLen);
    return writePacket(ch.cmdOutEndpoint, packet.data(), UsbPacketSize, timeoutMs);
}

std::array<uint8_t, UsbCanB::UsbPacketSize> UsbCanB::makeCommandPacket(Command cmd, const uint8_t* payload, int payloadLen)
{
    std::array<uint8_t, UsbPacketSize> p{};
    putLe32(p.data(), static_cast<uint32_t>(cmd));
    const int n = std::clamp(payloadLen, 0, UsbPacketSize - 4);
    if (payload && n > 0) {
        std::memcpy(p.data() + 4, payload, static_cast<size_t>(n));
    }
    return p;
}

std::array<uint8_t, UsbCanB::UsbPacketSize> UsbCanB::makeInitConfigPacket(Mode mode)
{
    std::array<uint8_t, UsbPacketSize> p{};
    putLe32(p.data() + 0, static_cast<uint32_t>(CmdInitCan));
    putLe32(p.data() + 4, 0x00000000u);
    putLe32(p.data() + 8, 0xffffffffu);
    putLe32(p.data() + 12, 0x00000000u);
    putLe32(p.data() + 16, 0x00000001u);
    putLe32(p.data() + 20, 0x00000000u);
    putLe32(p.data() + 24, 0x00000003u);
    putLe32(p.data() + 28, 0x0000001cu);
    putLe32(p.data() + 32, static_cast<uint32_t>(mode));
    return p;
}

int UsbCanB::transmit(const Channel& ch, const std::vector<Frame>& frames, int timeoutMs)
{
    int sent = 0;
    while (sent < static_cast<int>(frames.size())) {
        const int batch = std::min(3, static_cast<int>(frames.size()) - sent);
        const auto packet = packTxPacket(frames, sent, batch);
        if (!writePacket(ch.txOutEndpoint, packet.data(), UsbPacketSize, timeoutMs)) {
            return sent;
        }
        sent += batch;
    }
    return sent;
}

std::array<uint8_t, UsbCanB::UsbPacketSize> UsbCanB::packTxPacket(const std::vector<Frame>& frames, int first, int count)
{
    std::array<uint8_t, UsbPacketSize> p{};
    const int n = std::clamp(count, 0, 3);
    p[0] = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
        packFrame21(p.data() + 1 + i * 21, frames[static_cast<size_t>(first + i)]);
    }
    return p;
}

void UsbCanB::packFrame21(uint8_t* p, const Frame& frame)
{
    putLe32(p + 0, frame.id);
    putLe32(p + 4, 0);       // timestamp, TX ignored
    p[8] = 0;                // TimeFlag
    p[9] = 0;                // SendType normal
    p[10] = frame.remote ? 1 : 0;
    p[11] = frame.extended ? 1 : 0;
    p[12] = std::min<uint8_t>(frame.dlc, 8);
    for (int i = 0; i < 8; ++i) {
        p[13 + i] = frame.data[static_cast<size_t>(i)];
    }
}

UsbCanB::Frame UsbCanB::unpackFrame21(const uint8_t* p)
{
    Frame f{};
    f.id = getLe32(p + 0);
    f.remote = p[10] != 0;
    f.extended = p[11] != 0;
    f.dlc = std::min<uint8_t>(p[12], 8);
    for (int i = 0; i < 8; ++i) {
        f.data[static_cast<size_t>(i)] = p[13 + i];
    }
    return f;
}

std::vector<UsbCanB::Frame> UsbCanB::receive(const Channel& ch, int timeoutMs)
{
    std::array<uint8_t, UsbPacketSize> packet{};
    const int nread = readPacket(ch.rxInEndpoint, packet.data(), UsbPacketSize, timeoutMs, false);
    std::vector<Frame> out;
    if (nread <= 0) {
        return out;
    }

    const int count = std::min<int>(packet[0], 3);
    for (int i = 0; i < count; ++i) {
        const int off = 1 + i * 21;
        if (off + 21 <= nread) {
            out.push_back(unpackFrame21(packet.data() + off));
        }
    }
    return out;
}

bool UsbCanB::writePacket(uint8_t endpoint, const uint8_t* data, int len, int timeoutMs)
{
    if (!handle_) {
        lastError_ = "device not open";
        return false;
    }
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle_, endpoint, const_cast<uint8_t*>(data), len, &transferred, timeoutMs);
    if (rc != LIBUSB_SUCCESS || transferred != len) {
        setUsbError("Bulk write", endpoint, rc, transferred, len);
        if (rc == LIBUSB_ERROR_PIPE) {
            clearHalt(endpoint);
        }
        return false;
    }
    return true;
}

int UsbCanB::readPacket(uint8_t endpoint, uint8_t* data, int len, int timeoutMs, bool timeoutIsError)
{
    if (!handle_) {
        lastError_ = "device not open";
        return -1;
    }
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle_, endpoint, data, len, &transferred, timeoutMs);
    if (rc == LIBUSB_ERROR_TIMEOUT && !timeoutIsError) {
        return 0;
    }
    if (rc != LIBUSB_SUCCESS) {
        setUsbError("Bulk read", endpoint, rc, transferred, len);
        if (rc == LIBUSB_ERROR_PIPE) {
            clearHalt(endpoint);
        }
        return -1;
    }
    return transferred;
}

void UsbCanB::clearHalt(uint8_t endpoint)
{
    if (handle_) {
        libusb_clear_halt(handle_, endpoint);
    }
}

void UsbCanB::setUsbError(const char* what, uint8_t endpoint, int rc, int transferred, int requested)
{
    std::ostringstream os;
    os << what << " EP 0x" << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(endpoint) << std::dec
       << " error: " << libusb_error_name(rc)
       << ", transferred " << transferred << "/" << requested;
    lastError_ = os.str();
}
