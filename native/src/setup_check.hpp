// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// First-run setup check (qb_check_setup, qbhost --check, Unity's QuestBridge > Check Setup).
// setup_probe.cpp gathers the facts per platform; evaluate() turns them into checks with
// fix-it text, so the wording is tested without a headset.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ovr_service.hpp"

namespace qb::setup {

enum class Status { Ok = 0, Warn = 1, Fail = 2, Skip = 3 };

struct Check {
    std::string id;  // stable key, see questbridge.h
    std::string title;
    Status status = Status::Ok;
    std::string detail;  // what was found
    std::string fix;     // what to do; empty when OK
};

enum class Os { Windows, MacOS, Other };

enum class Claim {
    NotTried,  // QB_CHECK_NO_CLAIM, or no XRSP interface to try
    Ok,        // opened and released again
    Self,      // this process already holds it (streaming or waiting for Launch)
    Busy,      // another process holds it
    Failed,
};

enum class Encoder { Hardware, SoftwareOnly, None, NotChecked };

struct Probe {
    Os os = Os::Other;
    std::string os_version;  // "Windows 10.0.26200", "macOS 26.7.1"
    bool link_app = false;   // Windows: Meta Horizon Link installed (OVRService registered)
    bool quest = false;      // a device with Meta's vendor ID on USB
    uint16_t pid = 0;
    bool xrsp = false;       // its XRSP interface is visible
    Claim claim = Claim::NotTried;
    std::string claim_detail;
    ovr::ServiceState service = ovr::ServiceState::NotInstalled;
    bool auto_stop = true;   // QB_OVR_STOP isn't 0
    ovr::HelperState helper = ovr::HelperState::Idle;
    Encoder encoder = Encoder::NotChecked;
    std::string encoder_detail;  // encoder name, or why none opened
};

std::vector<Check> evaluate(const Probe& p);
// Worst status (Skip counts as Ok)
Status overall(const std::vector<Check>& checks);

}  // namespace qb::setup
