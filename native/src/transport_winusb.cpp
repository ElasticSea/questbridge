// Windows transport: opens just the Quest's XRSP interface through WinUSB (port of
// tools/pyhost/winusb_link.py). libusb on Windows opens every interface of a composite device, which
// fails while adb holds the ADB interface; WinUSB opens interface 0 alone via its path.
//
// All transfers are overlapped: we wait on an event with our own deadline and abort the pipe
// on timeout, so a call never blocks past timeout_ms and bytes that arrived are kept.
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

#include <atomic>
#include <cwchar>
#include <vector>

#include "ovr_service.hpp"
#include "transport.hpp"

namespace qb {

namespace {

// {f72fe0d4-cbcb-407d-8814-9ed673d0dd6b}: the XRSP interface's WinUSB device interface GUID
const GUID kXrspGuid = {0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};

std::atomic<int> g_open{0};

std::vector<std::wstring> find_xrsp_paths() {
    std::vector<std::wstring> paths;
    HDEVINFO info = SetupDiGetClassDevsW(&kXrspGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return paths;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &kXrspGuid, i, &data)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &data, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(info, &data, detail, need, nullptr, nullptr))
            paths.emplace_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(info);
    return paths;
}

class WinUsbTransport final : public Transport {
public:
    WinUsbTransport(HANDLE file, WINUSB_INTERFACE_HANDLE usb) : file_(file), usb_(usb) {
        ++g_open;
        ev_out_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ev_in_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (UCHAR ep : {kEpOut, kEpIn}) {
            ULONG none = 0;  // no driver-side timeout; transfer() enforces the deadline
            WinUsb_SetPipePolicy(usb_, ep, PIPE_TRANSFER_TIMEOUT, sizeof(none), &none);
        }
    }

    ~WinUsbTransport() override {
        WinUsb_AbortPipe(usb_, kEpOut);
        WinUsb_AbortPipe(usb_, kEpIn);
        WinUsb_Free(usb_);
        CloseHandle(file_);
        CloseHandle(ev_out_);
        CloseHandle(ev_in_);
        --g_open;
    }

    bool write(const uint8_t* data, size_t len, int timeout_ms) override {
        bool timed_out = false;
        long done = transfer(true, const_cast<uint8_t*>(data), len, timeout_ms, timed_out);
        return done == long(len) && !timed_out;
    }

    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        bool timed_out = false;
        return transfer(false, buf, cap, timeout_ms, timed_out);
    }

private:
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    HANDLE ev_out_, ev_in_;

    long transfer(bool out, uint8_t* buf, size_t len, int timeout_ms, bool& timed_out) {
        UCHAR ep = out ? kEpOut : kEpIn;
        OVERLAPPED ov{};
        ov.hEvent = out ? ev_out_ : ev_in_;
        ResetEvent(ov.hEvent);
        ULONG done = 0;
        BOOL ok = out ? WinUsb_WritePipe(usb_, ep, buf, ULONG(len), nullptr, &ov)
                      : WinUsb_ReadPipe(usb_, ep, buf, ULONG(len), nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return -1;
        DWORD r = WaitForSingleObject(ov.hEvent, timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
        if (r == WAIT_TIMEOUT) {
            timed_out = true;
            WinUsb_AbortPipe(usb_, ep);  // cancel, then wait so buf is no longer in use
        } else if (r != WAIT_OBJECT_0) {
            WinUsb_AbortPipe(usb_, ep);
            WinUsb_GetOverlappedResult(usb_, &ov, &done, TRUE);
            return -1;
        }
        ok = WinUsb_GetOverlappedResult(usb_, &ov, &done, TRUE);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_OPERATION_ABORTED && err != ERROR_SEM_TIMEOUT) return -1;
            timed_out = true;
        }
        return long(done);
    }
};

// Any present USB device with the Quest's vendor ID (instance IDs look like USB\VID_2833&PID_5013\...)
bool find_quest(uint16_t& pid) {
    HDEVINFO info = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(info, i, &dev); ++i) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(info, &dev, id, 512, nullptr)) continue;
        if (_wcsnicmp(id, L"USB\\VID_2833&PID_", 17) != 0) continue;
        found = true;
        pid = uint16_t(std::wcstoul(id + 17, nullptr, 16));
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

bool quest_present() {
    uint16_t pid;
    return find_quest(pid);
}

bool yes() { return true; }

// A failed open: let the OVRService helper see it and add its note to the log line
void ovr_note(std::string& detail, bool (*quest)()) {
    std::string note = ovr::on_open_failed(quest);
    if (!note.empty()) detail += "; " + note;
}

}  // namespace

UsbScan scan_usb() {
    UsbScan s;
    s.quest = find_quest(s.pid);
    s.xrsp = !find_xrsp_paths().empty();
    return s;
}

int open_transport_count() { return g_open; }

namespace {

std::unique_ptr<Transport> open_xrsp(OpenError& err, std::string& detail) {
    auto paths = find_xrsp_paths();
    if (paths.empty()) {
        err = OpenError::NotFound;
        detail = "Quest XRSP interface not found (plugged in? OVRService stopped?)";
        return nullptr;
    }
    HANDLE file = CreateFileW(paths[0].c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = OpenError::Busy;
        detail = "cannot open XRSP interface (error " + std::to_string(GetLastError()) +
                 "; is OVRService still running?)";
        return nullptr;
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        err = OpenError::Busy;
        detail = "WinUsb_Initialize failed (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(file);
        return nullptr;
    }
    err = OpenError::None;
    return std::make_unique<WinUsbTransport>(file, usb);
}

}  // namespace

std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    auto t = open_xrsp(err, detail);
    if (err == OpenError::NotFound) ovr_note(detail, quest_present);
    if (err == OpenError::Busy) ovr_note(detail, yes);
    return t;
}

OpenError try_claim(std::string& detail) {
    OpenError err;
    open_xrsp(err, detail);  // the transport closes again right away
    return err;
}

}  // namespace qb
