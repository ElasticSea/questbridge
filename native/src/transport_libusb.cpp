// libusb transport (macOS, Linux): claims only interface 0. The Quest's interfaces are all
// vendor class, so no kernel driver needs detaching on macOS.
#include <libusb.h>

#include "transport.hpp"

namespace ql {

namespace {

unsigned ms(int timeout_ms) { return unsigned(timeout_ms <= 0 ? 1 : timeout_ms); }  // libusb: 0 = forever

class LibusbTransport final : public Transport {
public:
    LibusbTransport(libusb_context* ctx, libusb_device_handle* h) : ctx_(ctx), h_(h) {}

    ~LibusbTransport() override {
        libusb_release_interface(h_, kXrspInterface);
        libusb_close(h_);
        libusb_exit(ctx_);
    }

    bool write(const uint8_t* data, size_t len, int timeout_ms) override {
        int done = 0;
        int r = libusb_bulk_transfer(h_, kEpOut, const_cast<uint8_t*>(data), int(len), &done, ms(timeout_ms));
        return r == 0 && size_t(done) == len;
    }

    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        int done = 0;
        int r = libusb_bulk_transfer(h_, kEpIn, buf, int(cap), &done, ms(timeout_ms));
        if (r == 0 || r == LIBUSB_ERROR_TIMEOUT) return done;  // bytes before a timeout are kept
        return -1;
    }

private:
    libusb_context* ctx_;
    libusb_device_handle* h_;
};

}  // namespace

std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        err = OpenError::Unsupported;
        detail = "libusb_init failed";
        return nullptr;
    }
    libusb_device_handle* h = nullptr;
    for (uint16_t pid : kQuestPids)
        if ((h = libusb_open_device_with_vid_pid(ctx, kQuestVid, pid))) break;
    if (!h) {
        libusb_exit(ctx);
        err = OpenError::NotFound;
        detail = "Quest not found (is it plugged in and awake?)";
        return nullptr;
    }
    int r = libusb_claim_interface(h, kXrspInterface);
    if (r != 0) {
        libusb_close(h);
        libusb_exit(ctx);
        err = OpenError::Busy;
        detail = std::string("cannot claim XRSP interface: ") + libusb_error_name(r);
        return nullptr;
    }
    err = OpenError::None;
    return std::make_unique<LibusbTransport>(ctx, h);
}

}  // namespace ql
