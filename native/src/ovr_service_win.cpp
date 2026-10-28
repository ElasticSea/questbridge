// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Windows half of ovr_service.hpp: SCM query and the elevated helper.
//
// The helper is powershell.exe started with the "runas" verb (one UAC prompt, hidden window).
// It stops OVRService, waits for this process to exit (Editor quit, player exit, crash) and
// starts the service again, so Meta Horizon Link comes back on its own.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "ovr_service.hpp"

namespace qb::ovr {

namespace {

const wchar_t* kService = L"OVRService";

std::mutex mu;
HelperState state = HelperState::Idle;  // guarded by mu
bool asked = false;                     // guarded by mu
std::chrono::steady_clock::time_point last_query;
ServiceState last_state = ServiceState::Unknown;

void set_state(HelperState s) {
    std::lock_guard<std::mutex> l(mu);
    state = s;
}

void run_helper() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string script = helper_script(GetCurrentProcessId(), "Stop-Service -Name OVRService -Force",
                                       "Start-Service -Name OVRService");
    std::string b64 = encode_command(script);
    std::wstring params = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand " +
                          std::wstring(b64.begin(), b64.end());
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys, n) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        set_state(GetLastError() == ERROR_CANCELLED ? HelperState::Declined : HelperState::Failed);
    } else {
        // The helper keeps running until we exit; done once the service reports stopped
        HelperState result = HelperState::Failed;
        for (int i = 0; i < 300; ++i) {  // 30 s
            if (query() == ServiceState::Stopped) {
                result = HelperState::Stopped;
                break;
            }
            if (sei.hProcess && WaitForSingleObject(sei.hProcess, 100) == WAIT_OBJECT_0) break;  // stop failed
            if (!sei.hProcess) Sleep(100);
        }
        set_state(result);
        if (sei.hProcess) CloseHandle(sei.hProcess);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

// Caller holds mu
void start_helper() {
    asked = true;
    state = HelperState::Prompting;
    std::thread(run_helper).detach();
}

}  // namespace

bool enabled() {
    const char* v = std::getenv("QB_OVR_STOP");
    return !(v && *v && std::atoi(v) == 0);
}

ServiceState query() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return ServiceState::Unknown;
    ServiceState s = ServiceState::Unknown;
    SC_HANDLE svc = OpenServiceW(scm, kService, SERVICE_QUERY_STATUS);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) s = ServiceState::NotInstalled;
    } else {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st)) {
            switch (st.dwCurrentState) {
                case SERVICE_STOPPED: s = ServiceState::Stopped; break;
                case SERVICE_RUNNING: s = ServiceState::Running; break;
                default: s = ServiceState::Pending; break;
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return s;
}

HelperState helper_state() {
    std::lock_guard<std::mutex> l(mu);
    return state;
}

std::string on_open_failed(bool (*quest_present)()) {
    if (!enabled()) return {};
    std::lock_guard<std::mutex> l(mu);
    // open_usb is retried every 50 ms: ask the SCM at most once a second
    auto now = std::chrono::steady_clock::now();
    if (now - last_query > std::chrono::seconds(1)) {
        last_state = query();
        last_query = now;
    }
    if (last_state != ServiceState::Running) return {};
    bool quest = quest_present();
    if (should_stop(true, last_state, quest, asked)) {
        start_helper();
        return "OVRService holds the Quest: asking to stop it (admin prompt); it restarts when this app exits";
    }
    if (!quest) return {};
    switch (state) {
        case HelperState::Prompting: return "waiting for the admin prompt to stop OVRService";
        case HelperState::Declined: return "OVRService holds the Quest and the admin prompt was declined (tools\\windows\\ovr_service.ps1 stop)";
        case HelperState::Failed: return "could not stop OVRService (tools\\windows\\ovr_service.ps1 stop)";
        default: return "OVRService is running again and holds the Quest";
    }
}

bool request_stop() {
    std::lock_guard<std::mutex> l(mu);
    last_state = query();
    last_query = std::chrono::steady_clock::now();
    if (last_state != ServiceState::Running) return false;
    if (state != HelperState::Prompting) start_helper();
    return true;
}

}  // namespace qb::ovr
