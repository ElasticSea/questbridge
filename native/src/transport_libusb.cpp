// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// libusb transport (macOS, Linux): claims only the XRSP interface, so adb keeps the ADB
// interface. The Quest's interfaces are all vendor class: macOS binds no kernel driver to
// them; on Linux a bound driver is detached while we hold the interface.
//
// Reopen after an unplug is the caller's job (the runtime retries open_usb every 250 ms),
// which works the same on every backend and needs no hotplug support from libusb.
#include <libusb.h>

#include <atomic>
#include <mutex>

#include "transport.hpp"

namespace qb {

namespace {

constexpr uint8_t kXrspClass = 0xff, kXrspSubclass = 0x8a;  // Meta's runtime: winusb,2833,ff,8A

std::atomic<int> g_open{0};

// libusb: 0 means wait forever. Ours (as WinUSB's backend): < 0 forever, 0 = poll.
unsigned ms(int timeout_ms) { return timeout_ms < 0 ? 0u : unsigned(timeout_ms == 0 ? 1 : timeout_ms); }

// One context for the process: open_usb is retried every 250 ms while no Quest is plugged in
libusb_context* context(std::string& detail) {
    static std::once_flag once;
    static libusb_context* ctx = nullptr;
    static int rc = 0;
    std::call_once(once, [] { rc = libusb_init(&ctx); });
    if (rc != 0) detail = std::string("libusb_init failed: ") + libusb_error_name(rc);
    return rc == 0 ? ctx : nullptr;
}

bool is_quest(const libusb_device_descriptor& d) {
    if (d.idVendor != kQuestVid) return false;
    for (uint16_t pid : kQuestPids)
        if (d.idProduct == pid) return true;
    return false;
}

// The XRSP interface must carry both bulk endpoints we use
bool has_xrsp_interface(libusb_device* dev) {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return false;
    bool ok = false;
    for (int i = 0; i < cfg->bNumInterfaces && !ok; ++i) {
        const libusb_interface& itf = cfg->interface[i];
        if (itf.num_altsetting < 1) continue;
        const libusb_interface_descriptor& alt = itf.altsetting[0];
        if (alt.bInterfaceNumber != kXrspInterface || alt.bInterfaceClass != kXrspClass ||
            alt.bInterfaceSubClass != kXrspSubclass)
            continue;
        bool in = false, out = false;
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = alt.endpoint[e];
            if ((ep.bmAttributes & 0x3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            in |= ep.bEndpointAddress == kEpIn;
            out |= ep.bEndpointAddress == kEpOut;
        }
        ok = in && out;
    }
    libusb_free_config_descriptor(cfg);
    return ok;
}

class LibusbTransport final : public Transport {
public:
    explicit LibusbTransport(libusb_device_handle* h) : h_(h) { ++g_open; }

    ~LibusbTransport() override {
        libusb_release_interface(h_, kXrspInterface);
        libusb_close(h_);
        --g_open;
    }

    bool write(const uint8_t* data, size_t len, int timeout_ms) override {
        int done = 0;
        int r = libusb_bulk_transfer(h_, kEpOut, const_cast<uint8_t*>(data), int(len), &done, ms(timeout_ms));
        return r == 0 && size_t(done) == len;
    }

    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        int done = 0;
        int r = libusb_bulk_transfer(h_, kEpIn, buf, int(cap), &done, ms(timeout_ms));
        if (r == 0 || r == LIBUSB_ERROR_TIMEOUT || r == LIBUSB_ERROR_INTERRUPTED)
            return done;  // bytes before a timeout are kept
        return -1;        // NO_DEVICE (unplugged), PIPE, OVERFLOW, IO
    }

private:
    libusb_device_handle* h_;
};

}  // namespace

UsbScan scan_usb() {
    UsbScan s;
    std::string detail;
    libusb_context* ctx = context(detail);
    if (!ctx) return s;
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != kQuestVid) continue;
        bool xrsp = is_quest(d) && has_xrsp_interface(list[i]);
        if (s.quest && !xrsp) continue;  // prefer the one with the XRSP interface
        s = {true, d.idProduct, xrsp};
    }
    if (n > 0) libusb_free_device_list(list, 1);
    return s;
}

int open_transport_count() { return g_open; }

OpenError try_claim(std::string& detail) {
    OpenError err;
    open_usb(err, detail);  // the transport closes again right away
    return err;
}

std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    libusb_context* ctx = context(detail);
    if (!ctx) {
        err = OpenError::Unsupported;
        return nullptr;
    }
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle* h = nullptr;
    err = OpenError::NotFound;
    detail = "Quest not found (plugged in, awake, Link selected?)";
    for (ssize_t i = 0; i < n && !h; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0 || !is_quest(d)) continue;
        if (!has_xrsp_interface(list[i])) {
            detail = "Quest found but without the XRSP interface (Link not active?)";
            continue;
        }
        int r = libusb_open(list[i], &h);
        if (r != 0) {
            err = OpenError::Busy;
            detail = std::string("cannot open Quest: ") + libusb_error_name(r);
            h = nullptr;
            continue;
        }
        libusb_set_auto_detach_kernel_driver(h, 1);  // Linux only; a no-op error elsewhere
        r = libusb_claim_interface(h, kXrspInterface);
        if (r != 0) {
            err = OpenError::Busy;
            detail = std::string("cannot claim XRSP interface: ") + libusb_error_name(r);
            libusb_close(h);
            h = nullptr;
        }
    }
    if (n > 0) libusb_free_device_list(list, 1);
    if (!h) return nullptr;
    err = OpenError::None;
    detail.clear();
    return std::make_unique<LibusbTransport>(h);
}

}  // namespace qb
