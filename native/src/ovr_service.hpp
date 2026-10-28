// Meta's OVRService (Windows) holds the Quest's XRSP interface while it runs. When a USB open
// fails because of it, the host stops it through one elevated helper (one UAC prompt per
// process) that starts it again when the process exits. QL_OVR_STOP=0 turns this off.
#pragma once

#include <string>

namespace ql::ovr {

enum class ServiceState { NotInstalled, Stopped, Running, Pending, Unknown };

const char* to_string(ServiceState s);

// Whether a failed USB open should ask to stop OVRService
bool should_stop(bool enabled, ServiceState state, bool quest_present, bool already_asked);

// PowerShell for the elevated helper: runs stop_cmd, waits for parent_pid to exit, runs
// start_cmd. A failing stop_cmd ends it without start_cmd.
std::string helper_script(unsigned long parent_pid, const std::string& stop_cmd, const std::string& start_cmd);
// powershell.exe -EncodedCommand payload: base64 of the UTF-16LE script
std::string encode_command(const std::string& script);

// What the helper did so far in this process
enum class HelperState { Idle, Prompting, Stopped, Declined, Failed };
const char* to_string(HelperState s);

// Platform side (ovr_service_win.cpp; elsewhere OVRService doesn't exist)
bool enabled();               // QL_OVR_STOP != 0
ServiceState query();         // SCM, no elevation
HelperState helper_state();
// Starts the helper if should_stop() says so; never blocks. quest_present is only called while
// OVRService runs. Returns a note for the USB log (empty if nothing to say).
std::string on_open_failed(bool (*quest_present)());
// Ask now (Check Setup's button), even after a declined prompt. False if OVRService isn't running.
bool request_stop();

}  // namespace ql::ovr
