// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Platform-neutral half of ovr_service.hpp (tested on every OS)
#include "ovr_service.hpp"

#include <cstdint>

namespace qb::ovr {

const char* to_string(ServiceState s) {
    switch (s) {
        case ServiceState::NotInstalled: return "not installed";
        case ServiceState::Stopped: return "stopped";
        case ServiceState::Running: return "running";
        case ServiceState::Pending: return "starting or stopping";
        default: return "unknown";
    }
}

const char* to_string(HelperState s) {
    switch (s) {
        case HelperState::Idle: return "idle";
        case HelperState::Prompting: return "waiting for the admin prompt";
        case HelperState::Stopped: return "stopped by QuestBridge";
        case HelperState::Declined: return "admin prompt declined";
        default: return "failed";
    }
}

bool should_stop(bool enabled, ServiceState state, bool quest_present, bool already_asked) {
    return enabled && quest_present && !already_asked && state == ServiceState::Running;
}

std::string helper_script(unsigned long parent_pid, const std::string& stop_cmd, const std::string& start_cmd) {
    std::string pid = std::to_string(parent_pid);
    return "$ErrorActionPreference = 'Stop'\n"
           "$parent = Get-Process -Id " + pid + " -ErrorAction SilentlyContinue\n" +
           stop_cmd + "\n"
           "if ($parent) { $parent.WaitForExit() }\n" +
           start_cmd + "\n";
}

std::string encode_command(const std::string& script) {
    // ASCII script -> UTF-16LE bytes -> base64
    std::string bytes;
    for (unsigned char c : script) {
        bytes.push_back(char(c));
        bytes.push_back('\0');
    }
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        uint32_t v = uint32_t(uint8_t(bytes[i])) << 16 | uint32_t(uint8_t(bytes[i + 1])) << 8 | uint8_t(bytes[i + 2]);
        out += {abc[v >> 18], abc[(v >> 12) & 63], abc[(v >> 6) & 63], abc[v & 63]};
    }
    if (size_t rest = bytes.size() - i) {
        uint32_t v = uint32_t(uint8_t(bytes[i])) << 16 | (rest > 1 ? uint32_t(uint8_t(bytes[i + 1])) << 8 : 0);
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += rest > 1 ? abc[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

#if !defined(_WIN32)
bool enabled() { return false; }
ServiceState query() { return ServiceState::NotInstalled; }
HelperState helper_state() { return HelperState::Idle; }
std::string on_open_failed(bool (*)()) { return {}; }
bool request_stop() { return false; }
#endif

}  // namespace qb::ovr
