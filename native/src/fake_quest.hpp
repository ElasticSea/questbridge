// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Offline bench headset (QB_FAKE_QUEST=<file>, see tools/make_fake_quest.py): replays a
// recorded Quest side (handshake, then a head pose with a slow yaw sweep at ~500 Hz) and
// measures what the host writes: video rate (average, peak 1 s), largest message per topic,
// messages over the cap. No USB.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "questbridge.h"

namespace qb {

struct FakeQuest;

// The transport for qb_host_open_transport, or nullptr (with *error set) if the file doesn't
// load. log gets one stats line every 10 s; max_message = the cap to count messages against.
std::shared_ptr<FakeQuest> load_fake_quest(const std::string& path, size_t max_message,
                                           std::function<void(const std::string&)> log, std::string* error);
qb_transport fake_quest_transport(const std::shared_ptr<FakeQuest>& quest);

}  // namespace qb
