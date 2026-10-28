// Windows: runs the real OVRService helper script, unelevated and with logging stand-ins for
// Stop-Service / Start-Service, to check the stop -> wait for the parent -> start sequence.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>

#include "harness.hpp"
#include "ovr_service.hpp"

namespace {

// Hidden child process (no console window); returns its handle, or null
HANDLE spawn(std::wstring cmd) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

HANDLE run_helper(const std::string& script) {
    std::string b64 = qb::ovr::encode_command(script);
    return spawn(L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
                 std::wstring(b64.begin(), b64.end()));
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::string s, line;
    while (std::getline(f, line)) s += line + "\n";
    return s;
}

struct Log {
    std::filesystem::path path;
    Log() {
        path = std::filesystem::temp_directory_path() / ("qb_ovr_helper_" + std::to_string(GetCurrentProcessId()) + ".txt");
        std::filesystem::remove(path);
    }
    ~Log() { std::filesystem::remove(path); }
    std::string cmd(const char* word) const { return "Add-Content -LiteralPath '" + path.string() + "' -Value " + word; }
};

}  // namespace

TEST(ovr_helper_stops_waits_for_parent_then_starts) {
    Log log;
    HANDLE parent = spawn(L"cmd.exe /c ping -n 5 127.0.0.1 >nul");  // ~4 s
    REQUIRE(parent);
    HANDLE helper = run_helper(qb::ovr::helper_script(GetProcessId(parent), log.cmd("stop"), log.cmd("start")));
    REQUIRE(helper);
    // Stopped, and still waiting while the parent lives
    for (int i = 0; i < 100 && read_all(log.path).empty(); ++i) Sleep(50);
    CHECK_EQ(read_all(log.path), std::string("stop\n"));
    CHECK_EQ(WaitForSingleObject(helper, 0), DWORD(WAIT_TIMEOUT));
    TerminateProcess(parent, 0);  // the "Editor" exits
    CHECK_EQ(WaitForSingleObject(helper, 15000), DWORD(WAIT_OBJECT_0));
    CHECK_EQ(read_all(log.path), std::string("stop\nstart\n"));
    CloseHandle(helper);
    CloseHandle(parent);
}

TEST(ovr_helper_failed_stop_skips_start) {
    Log log;
    HANDLE helper = run_helper(qb::ovr::helper_script(GetCurrentProcessId(), "throw 'no admin'", log.cmd("start")));
    REQUIRE(helper);
    CHECK_EQ(WaitForSingleObject(helper, 15000), DWORD(WAIT_OBJECT_0));
    DWORD code = 0;
    GetExitCodeProcess(helper, &code);
    CHECK(code != 0);
    CHECK_EQ(read_all(log.path), std::string());
    CloseHandle(helper);
}

TEST(ovr_helper_parent_already_gone_starts_right_away) {
    Log log;
    HANDLE parent = spawn(L"cmd.exe /c exit 0");
    REQUIRE(parent);
    WaitForSingleObject(parent, 5000);
    DWORD pid = GetProcessId(parent);
    CloseHandle(parent);  // pid now names no process (or an unrelated one only after reuse)
    HANDLE helper = run_helper(qb::ovr::helper_script(pid, log.cmd("stop"), log.cmd("start")));
    REQUIRE(helper);
    CHECK_EQ(WaitForSingleObject(helper, 15000), DWORD(WAIT_OBJECT_0));
    CHECK_EQ(read_all(log.path), std::string("stop\nstart\n"));
    CloseHandle(helper);
}
