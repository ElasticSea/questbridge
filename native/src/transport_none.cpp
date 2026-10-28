// Fallback when no USB backend is available at build time (e.g. libusb not installed).
#include "transport.hpp"

namespace ql {

std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    err = OpenError::Unsupported;
    detail = "built without a USB backend (install libusb-1.0 and reconfigure)";
    return nullptr;
}

UsbScan scan_usb() { return {}; }

OpenError try_claim(std::string& detail) {
    detail = "built without a USB backend";
    return OpenError::Unsupported;
}

int open_transport_count() { return 0; }

}  // namespace ql
