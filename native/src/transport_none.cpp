// Fallback when no USB backend is available at build time (e.g. libusb not installed).
#include "transport.hpp"

namespace ql {

std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    err = OpenError::Unsupported;
    detail = "built without a USB backend (install libusb-1.0 and reconfigure)";
    return nullptr;
}

}  // namespace ql
