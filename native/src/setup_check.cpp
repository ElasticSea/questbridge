#include "setup_check.hpp"

#include <cstdio>

namespace ql::setup {

namespace {

const char* kServices = "stop \"Oculus VR Runtime Service\" in services.msc (needs admin)";

Check make(const char* id, const char* title, Status s, std::string detail, std::string fix = {}) {
    return {id, title, s, std::move(detail), std::move(fix)};
}

Check os_check(const Probe& p) {
    if (p.os == Os::Other)
        return make("os", "Operating system", Status::Fail, p.os_version.empty() ? "unsupported" : p.os_version,
                    "QuestLink runs on Windows 10/11 (x64) and on macOS with Apple silicon.");
    return make("os", "Operating system", Status::Ok, p.os_version);
}

Check link_app_check(const Probe& p) {
    const char* title = "Meta Horizon Link (USB driver)";
    if (p.os != Os::Windows) return make("link_app", title, Status::Skip, "not needed on this OS");
    if (p.link_app) return make("link_app", title, Status::Ok, "installed");
    if (p.xrsp) return make("link_app", title, Status::Ok, "not installed, but its USB driver is");
    return make("link_app", title, Status::Fail, "not installed",
                "Install the Meta Quest Link app (Meta Horizon Link) from meta.com/help/quest/pcvr. QuestLink only "
                "needs the USB driver it installs; you don't have to sign in or keep it open. Then unplug and "
                "replug the Quest.");
}

Check quest_check(const Probe& p) {
    const char* title = "Quest on USB";
    if (p.quest) {
        char pid[16];
        std::snprintf(pid, sizeof pid, "0x%04x", unsigned(p.pid));
        return make("quest_usb", title, Status::Ok, std::string("found (USB product ID ") + pid + ")");
    }
    return make("quest_usb", title, Status::Fail, "no Quest found",
                "Plug the Quest into a USB 3 port of this computer with a data cable (the one in the box works), "
                "directly rather than through a hub. Press the headset's power button or put it on so it's awake.");
}

Check xrsp_check(const Probe& p) {
    const char* title = "Link interface";
    if (!p.quest) return make("xrsp_interface", title, Status::Skip, "needs the Quest on USB");
    if (p.xrsp) return make("xrsp_interface", title, Status::Ok, "present");
    if (p.os == Os::Windows && !p.link_app)
        return make("xrsp_interface", title, Status::Fail, "the Quest's Link interface has no driver",
                    "Install Meta Horizon Link (see above), then unplug and replug the Quest.");
    return make("xrsp_interface", title, Status::Fail, "the Quest is on USB but doesn't offer its Link interface",
                "In the headset, turn Link on (Settings > System > Quest Link). If it was on, unplug and replug "
                "the cable once.");
}

Check service_check(const Probe& p) {
    using ovr::HelperState;
    using ovr::ServiceState;
    const char* title = "Meta OVRService";
    if (p.os != Os::Windows) return make("ovr_service", title, Status::Skip, "Windows only");
    switch (p.service) {
        case ServiceState::NotInstalled: return make("ovr_service", title, Status::Skip, "not installed");
        case ServiceState::Stopped:
            return make("ovr_service", title, Status::Ok,
                        p.helper == HelperState::Stopped ? "stopped by QuestLink; it starts again when this app exits"
                                                         : "stopped");
        case ServiceState::Pending:
            return make("ovr_service", title, Status::Warn, "starting or stopping", "Check again in a few seconds.");
        case ServiceState::Unknown:
            return make("ovr_service", title, Status::Warn, "couldn't query its state", "If streaming doesn't start, " + std::string(kServices) + ".");
        case ServiceState::Running: break;
    }
    if (!p.auto_stop)
        return make("ovr_service", title, Status::Fail, "running, and QL_OVR_STOP=0 keeps QuestLink from stopping it",
                    "It holds the Quest's Link interface: " + std::string(kServices) + ", or unset QL_OVR_STOP.");
    switch (p.helper) {
        case HelperState::Prompting:
            return make("ovr_service", title, Status::Warn, "running; waiting for you to answer the admin prompt",
                        "Allow the Windows prompt (Windows PowerShell) so QuestLink can stop it.");
        case HelperState::Declined:
            return make("ovr_service", title, Status::Fail, "running; the admin prompt was declined",
                        "Use Stop OVRService (or restart Unity) and allow the prompt, or " + std::string(kServices) + ".");
        case HelperState::Failed:
            return make("ovr_service", title, Status::Fail, "running; QuestLink couldn't stop it",
                        std::string(kServices) + ".");
        default:
            return make("ovr_service", title, Status::Warn,
                        "running (Meta Link holds the Quest while it runs)",
                        "Nothing to do: once the Quest is plugged in, QuestLink asks once for admin rights (a Windows "
                        "prompt for Windows PowerShell), stops it and starts it again when this app exits. To skip "
                        "the prompt, " + std::string(kServices) + " before you start.");
    }
}

Check claim_check(const Probe& p) {
    const char* title = "Link interface free";
    if (!p.xrsp) return make("xrsp_claim", title, Status::Skip, "needs the Link interface");
    switch (p.claim) {
        case Claim::NotTried: return make("xrsp_claim", title, Status::Skip, "skipped on request");
        case Claim::Ok: return make("xrsp_claim", title, Status::Ok, "free");
        case Claim::Self: return make("xrsp_claim", title, Status::Ok, "in use by this app (QuestLink has the Quest)");
        case Claim::Failed:
            return make("xrsp_claim", title, Status::Fail, p.claim_detail.empty() ? "couldn't open it" : p.claim_detail,
                        "Unplug and replug the Quest, then check again.");
        case Claim::Busy: break;
    }
    if (p.os == Os::Windows && p.service == ovr::ServiceState::Running)
        return make("xrsp_claim", title, p.auto_stop ? Status::Warn : Status::Fail, "held by Meta's OVRService",
                    "See Meta OVRService above.");
    return make("xrsp_claim", title, Status::Fail, "another program has it open",
                "Close other QuestLink apps (a second Unity Editor in Play mode, a QuestLink player, qlhost) and "
                "Meta Horizon Link, then check again.");
}

Check encoder_check(const Probe& p) {
    const char* title = "GPU video encoder";
    const char* fix = "Streaming needs a hardware H.264 encoder (NVIDIA NVENC, AMD VCN, Intel Quick Sync). Update the "
                      "GPU driver; on a laptop, run Unity on the dedicated GPU (Windows Settings > Display > Graphics).";
    switch (p.encoder) {
        case Encoder::Hardware: return make("gpu_encoder", title, Status::Ok, p.encoder_detail);
        case Encoder::SoftwareOnly:
            return make("gpu_encoder", title, Status::Warn, "only a software encoder: " + p.encoder_detail, fix);
        case Encoder::None:
            return make("gpu_encoder", title, Status::Fail, "none: " + p.encoder_detail, fix);
        default: return make("gpu_encoder", title, Status::Skip, "not checked");
    }
}

}  // namespace

std::vector<Check> evaluate(const Probe& p) {
    return {os_check(p),      link_app_check(p), quest_check(p),  xrsp_check(p),
            service_check(p), claim_check(p),    encoder_check(p)};
}

Status overall(const std::vector<Check>& checks) {
    Status worst = Status::Ok;
    for (const auto& c : checks)
        if (c.status != Status::Skip && int(c.status) > int(worst)) worst = c.status;
    return worst;
}

}  // namespace ql::setup
