// ql_check_setup: gathers setup_check.hpp's Probe on this machine and evaluates it.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "questlink.h"
#include "setup_check.hpp"
#include "transport.hpp"

#if defined(_WIN32) || defined(__APPLE__)
#  include "encoder.hpp"
#endif

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <d3d11.h>
#  include <wrl/client.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#endif

// Layout the C# side reads (Editor/QuestLinkCheckSetup.cs)
static_assert(sizeof(ql_check) == 868 && offsetof(ql_check, fix) == 356);

namespace {

using namespace ql;
using setup::Claim;
using setup::Encoder;

void probe_os(setup::Probe& p) {
#if defined(_WIN32)
    p.os = setup::Os::Windows;
    // GetVersionEx lies to unmanifested callers; RtlGetVersion doesn't
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof(v)};
    if (fn && fn(&v) == 0)
        p.os_version = "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
                       std::to_string(v.dwBuildNumber);
    else
        p.os_version = "Windows";
#elif defined(__APPLE__)
    p.os = setup::Os::MacOS;
    char ver[64] = {};
    size_t len = sizeof ver;
    p.os_version = sysctlbyname("kern.osproductversion", ver, &len, nullptr, 0) == 0 ? "macOS " + std::string(ver) : "macOS";
#  if defined(__arm64__)
    p.os_version += " (Apple silicon)";
#  endif
#else
    p.os = setup::Os::Other;
#endif
}

void probe_encoder(setup::Probe& p) {
#if defined(_WIN32) || defined(__APPLE__)
    enc::EncoderConfig cfg;  // the stream's size: 4096x2240, 5 slices
    cfg.allow_software = false;
#  if defined(_WIN32)
    // Hardware MFTs take D3D11 input, as in the texture encoder
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        cfg.gpu_device = device.Get();
#  else
    cfg.layout = enc::Layout::Strips;  // as texture_encoder_metal.mm
#  endif
    std::string err;
    if (auto e = enc::create_encoder(cfg, &err)) {
        p.encoder = Encoder::Hardware;
        p.encoder_detail = e->name();
        return;
    }
    cfg.hardware = false;
    cfg.allow_software = true;
    std::string sw_err;
    if (auto e = enc::create_encoder(cfg, &sw_err)) {
        p.encoder = Encoder::SoftwareOnly;
        p.encoder_detail = e->name();
        return;
    }
    p.encoder = Encoder::None;
    p.encoder_detail = err.empty() ? sw_err : err;
#else
    (void)p;
#endif
}

void copy(char* dst, size_t cap, const std::string& s) {
    size_t n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

}  // namespace

extern "C" {

int ql_check_setup(uint32_t flags, ql_check* out, int cap) {
    if (cap > 0 && !out) return QL_ERR_ARGUMENT;
    setup::Probe p;
    try {
        probe_os(p);
        p.service = ovr::query();
        p.link_app = p.service != ovr::ServiceState::NotInstalled;
        p.auto_stop = ovr::enabled();
        p.helper = ovr::helper_state();
        UsbScan usb = scan_usb();
        p.quest = usb.quest;
        p.pid = usb.pid;
        p.xrsp = usb.xrsp;
        if (p.xrsp && open_transport_count() > 0) {
            p.claim = Claim::Self;
        } else if (p.xrsp && !(flags & QL_CHECK_NO_CLAIM)) {
            OpenError err = try_claim(p.claim_detail);
            p.claim = err == OpenError::None ? Claim::Ok : err == OpenError::Busy ? Claim::Busy : Claim::Failed;
            if (err == OpenError::NotFound) p.claim_detail = "the Link interface went away (Link restarting?)";
        }
        probe_encoder(p);
    } catch (...) {
        return QL_ERR_INTERNAL;
    }
    auto checks = setup::evaluate(p);
    for (int i = 0; i < cap && i < int(checks.size()); ++i) {
        const auto& c = checks[size_t(i)];
        out[i].status = int32_t(c.status);
        copy(out[i].id, sizeof out[i].id, c.id);
        copy(out[i].title, sizeof out[i].title, c.title);
        copy(out[i].detail, sizeof out[i].detail, c.detail);
        copy(out[i].fix, sizeof out[i].fix, c.fix);
    }
    return int(checks.size());
}

int ql_stop_ovr_service(void) {
#if defined(_WIN32)
    return ovr::request_stop() ? 1 : 0;
#else
    return QL_ERR_UNSUPPORTED;
#endif
}

}  // extern "C"
