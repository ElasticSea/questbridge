// Byte pipe to the Quest's XRSP interface (interface 0: bulk OUT 0x01, bulk IN 0x81).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace qb {

constexpr uint16_t kQuestVid = 0x2833;
constexpr uint16_t kQuestPids[] = {0x5010, 0x5011, 0x5012, 0x5013, 0x5017, 0x5018, 0x5019, 0x501A};
constexpr uint8_t kXrspInterface = 0, kEpOut = 0x01, kEpIn = 0x81;
// The Quest pads transfers to 1 KiB and sends no short packets; larger reads stall and lose
// data on timeout.
constexpr size_t kReadSize = 1024;

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool write(const uint8_t* data, size_t len, int timeout_ms) = 0;
    // Bytes read (partial data before a timeout is kept), 0 on timeout, -1 on error
    virtual long read(uint8_t* buf, size_t cap, int timeout_ms) = 0;
};

enum class OpenError { None, NotFound, Busy, Unsupported };

// Platform USB backend: WinUSB on Windows (opens interface 0 only, so adb keeps the ADB
// interface), libusb elsewhere.
std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail);

// What's on the bus, without opening anything (setup check)
struct UsbScan {
    bool quest = false;  // a device with the Quest's vendor ID
    uint16_t pid = 0;
    bool xrsp = false;   // its XRSP interface is visible (Windows: Meta's driver is bound to it)
};
UsbScan scan_usb();
// Open the XRSP interface and release it again (no OVRService prompt): None if it's free
OpenError try_claim(std::string& detail);
// XRSP transports this process holds open right now
int open_transport_count();

}  // namespace qb
