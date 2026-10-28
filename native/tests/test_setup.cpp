// Setup check wording/logic (setup_check.cpp) and the OVRService helper's pure parts.
#include "harness.hpp"
#include "ovr_service.hpp"
#include "setup_check.hpp"

using namespace qb;
using setup::Check;
using setup::Claim;
using setup::Encoder;
using setup::Probe;
using setup::Status;
using ovr::HelperState;
using ovr::ServiceState;

namespace {

// A Windows PC ready to stream: Link installed, OVRService stopped, Quest plugged in and free
Probe ready_windows() {
    Probe p;
    p.os = setup::Os::Windows;
    p.os_version = "Windows 10.0.26200";
    p.link_app = true;
    p.quest = true;
    p.pid = 0x5013;
    p.xrsp = true;
    p.claim = Claim::Ok;
    p.service = ServiceState::Stopped;
    p.encoder = Encoder::Hardware;
    p.encoder_detail = "NVIDIA H.264 Encoder MFT (hardware)";
    return p;
}

Probe ready_mac() {
    Probe p = ready_windows();
    p.os = setup::Os::MacOS;
    p.os_version = "macOS 26.7.1 (Apple silicon)";
    p.link_app = false;
    p.service = ServiceState::NotInstalled;
    p.encoder_detail = "VideoToolbox";
    return p;
}

// By value: callers pass evaluate()'s temporary
Check get(const std::vector<Check>& cs, const std::string& id) {
    for (const auto& c : cs)
        if (c.id == id) return c;
    return {"missing", "", Status::Fail, "", ""};
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(setup_order_and_ids_match_header) {
    auto cs = setup::evaluate(ready_windows());
    std::vector<std::string> ids;
    for (const auto& c : cs) ids.push_back(c.id);
    CHECK(ids == (std::vector<std::string>{"os", "link_app", "quest_usb", "xrsp_interface", "ovr_service", "xrsp_claim",
                                           "gpu_encoder"}));
    for (const auto& c : cs) {
        CHECK(c.id.size() < 32 && c.title.size() < 64 && c.detail.size() < 256 && c.fix.size() < 512);
        CHECK(!c.title.empty() && !c.detail.empty());
    }
}

TEST(setup_ready_windows_all_ok) {
    auto cs = setup::evaluate(ready_windows());
    CHECK_EQ(setup::overall(cs), Status::Ok);
    for (const auto& c : cs) CHECK(c.status == Status::Ok);
    CHECK(has(get(cs, "quest_usb").detail, "0x5013"));
    CHECK(has(get(cs, "gpu_encoder").detail, "NVIDIA"));
}

TEST(setup_ready_mac_skips_windows_checks) {
    auto cs = setup::evaluate(ready_mac());
    CHECK_EQ(setup::overall(cs), Status::Ok);
    CHECK_EQ(get(cs, "link_app").status, Status::Skip);
    CHECK_EQ(get(cs, "ovr_service").status, Status::Skip);
    CHECK_EQ(get(cs, "xrsp_claim").status, Status::Ok);
}

TEST(setup_no_quest) {
    Probe p = ready_windows();
    p.quest = false;
    p.xrsp = false;
    p.claim = Claim::NotTried;
    auto cs = setup::evaluate(p);
    CHECK_EQ(setup::overall(cs), Status::Fail);
    CHECK_EQ(get(cs, "quest_usb").status, Status::Fail);
    CHECK(has(get(cs, "quest_usb").fix, "USB 3"));
    CHECK_EQ(get(cs, "xrsp_interface").status, Status::Skip);  // follow-ups don't pile on
    CHECK_EQ(get(cs, "xrsp_claim").status, Status::Skip);
}

TEST(setup_windows_without_meta_link) {
    Probe p = ready_windows();
    p.link_app = false;
    p.service = ServiceState::NotInstalled;
    p.xrsp = false;
    p.claim = Claim::NotTried;
    auto cs = setup::evaluate(p);
    CHECK_EQ(get(cs, "link_app").status, Status::Fail);
    CHECK(has(get(cs, "link_app").fix, "meta.com"));
    CHECK_EQ(get(cs, "xrsp_interface").status, Status::Fail);
    CHECK(has(get(cs, "xrsp_interface").fix, "Install Meta Horizon Link"));
    CHECK_EQ(get(cs, "ovr_service").status, Status::Skip);
}

TEST(setup_driver_left_after_uninstall_is_fine) {
    Probe p = ready_windows();
    p.link_app = false;
    p.service = ServiceState::NotInstalled;
    auto cs = setup::evaluate(p);
    CHECK_EQ(get(cs, "link_app").status, Status::Ok);
    CHECK_EQ(setup::overall(cs), Status::Ok);
}

TEST(setup_quest_without_link_interface) {
    Probe p = ready_mac();
    p.xrsp = false;
    p.claim = Claim::NotTried;
    auto cs = setup::evaluate(p);
    CHECK_EQ(get(cs, "xrsp_interface").status, Status::Fail);
    CHECK(has(get(cs, "xrsp_interface").fix, "Quest Link"));
}

TEST(setup_ovr_service_running_is_a_warning_with_auto_stop) {
    Probe p = ready_windows();
    p.service = ServiceState::Running;
    p.claim = Claim::Busy;
    auto cs = setup::evaluate(p);
    CHECK_EQ(setup::overall(cs), Status::Warn);
    CHECK_EQ(get(cs, "ovr_service").status, Status::Warn);
    CHECK(has(get(cs, "ovr_service").fix, "admin"));
    CHECK_EQ(get(cs, "xrsp_claim").status, Status::Warn);
    CHECK(has(get(cs, "xrsp_claim").detail, "OVRService"));
}

TEST(setup_ovr_service_running_without_auto_stop_fails) {
    Probe p = ready_windows();
    p.service = ServiceState::Running;
    p.auto_stop = false;
    p.claim = Claim::Busy;
    auto cs = setup::evaluate(p);
    CHECK_EQ(get(cs, "ovr_service").status, Status::Fail);
    CHECK(has(get(cs, "ovr_service").fix, "services.msc"));
    CHECK_EQ(get(cs, "xrsp_claim").status, Status::Fail);
}

TEST(setup_ovr_helper_states) {
    Probe p = ready_windows();
    p.service = ServiceState::Running;
    p.helper = HelperState::Declined;
    CHECK_EQ(get(setup::evaluate(p), "ovr_service").status, Status::Fail);
    p.helper = HelperState::Prompting;
    CHECK_EQ(get(setup::evaluate(p), "ovr_service").status, Status::Warn);
    p.service = ServiceState::Stopped;
    p.helper = HelperState::Stopped;
    auto c = get(setup::evaluate(p), "ovr_service");
    CHECK_EQ(c.status, Status::Ok);
    CHECK(has(c.detail, "starts again"));
}

TEST(setup_interface_held_by_another_program) {
    Probe p = ready_mac();
    p.claim = Claim::Busy;
    auto c = get(setup::evaluate(p), "xrsp_claim");
    CHECK_EQ(c.status, Status::Fail);
    CHECK(has(c.fix, "second Unity Editor"));
}

TEST(setup_interface_held_by_this_process_is_ok) {
    Probe p = ready_windows();
    p.claim = Claim::Self;
    CHECK_EQ(get(setup::evaluate(p), "xrsp_claim").status, Status::Ok);
    p.claim = Claim::NotTried;
    CHECK_EQ(get(setup::evaluate(p), "xrsp_claim").status, Status::Skip);
}

TEST(setup_encoder) {
    Probe p = ready_windows();
    p.encoder = Encoder::SoftwareOnly;
    p.encoder_detail = "H264 Encoder MFT (software)";
    auto c = get(setup::evaluate(p), "gpu_encoder");
    CHECK_EQ(c.status, Status::Warn);
    CHECK(has(c.fix, "GPU driver"));
    p.encoder = Encoder::None;
    p.encoder_detail = "no H.264 MFT";
    c = get(setup::evaluate(p), "gpu_encoder");
    CHECK_EQ(c.status, Status::Fail);
    CHECK(has(c.detail, "no H.264 MFT"));
}

TEST(setup_overall_ignores_skip) {
    std::vector<Check> cs = {{"a", "A", Status::Skip, "x", ""}, {"b", "B", Status::Ok, "y", ""}};
    CHECK_EQ(setup::overall(cs), Status::Ok);
    cs.push_back({"c", "C", Status::Warn, "z", "do"});
    CHECK_EQ(setup::overall(cs), Status::Warn);
}

TEST(ovr_should_stop) {
    CHECK(ovr::should_stop(true, ServiceState::Running, true, false));
    CHECK(!ovr::should_stop(false, ServiceState::Running, true, false));  // QB_OVR_STOP=0
    CHECK(!ovr::should_stop(true, ServiceState::Stopped, true, false));
    CHECK(!ovr::should_stop(true, ServiceState::NotInstalled, true, false));
    CHECK(!ovr::should_stop(true, ServiceState::Running, false, false));  // no Quest: no prompt
    CHECK(!ovr::should_stop(true, ServiceState::Running, true, true));    // one prompt per process
}

TEST(ovr_helper_script) {
    std::string s = ovr::helper_script(1234, "Stop-Service -Name OVRService -Force", "Start-Service -Name OVRService");
    size_t stop = s.find("Stop-Service"), wait = s.find("WaitForExit"), start = s.find("Start-Service");
    CHECK(s.find("Get-Process -Id 1234") < stop);
    CHECK(stop < wait && wait < start && start != std::string::npos);
    CHECK(has(s, "$ErrorActionPreference = 'Stop'"));  // a failed stop skips the start
}

TEST(ovr_encode_command_is_utf16le_base64) {
    CHECK_EQ(ovr::encode_command(""), std::string());
    CHECK_EQ(ovr::encode_command("a"), std::string("YQA="));
    CHECK_EQ(ovr::encode_command("ab"), std::string("YQBiAA=="));
    CHECK_EQ(ovr::encode_command("abc"), std::string("YQBiAGMA"));
    CHECK_EQ(ovr::encode_command("dir"), std::string("ZABpAHIA"));
}
