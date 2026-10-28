#include "fake_quest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "capnp.hpp"
#include "xrsp.hpp"

namespace qb {

struct FakeQuest {
    std::mutex mu;
    Bytes rx;  // packed transfers not read yet
    size_t rx_pos = 0;
    Bytes pose_table, pose;  // head pose template (segment table, segment)
    size_t quat_at = 0;      // orientation (f32 xyzw) in pose
    uint16_t pose_seq = 0;
    int64_t start_ns = 0, last_pose_ns = 0;

    size_t max_message = 0;
    std::function<void(const std::string&)> log;
    Reassembler written;
    std::map<uint8_t, size_t> largest;  // per topic, whole run
    uint64_t over_cap = 0, messages = 0;
    uint64_t video_bytes = 0, second_bytes = 0, peak_second_bytes = 0;
    int64_t window_since = 0, second_since = 0;
};

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void queue(FakeQuest& q, const Bytes& raw) {
    Bytes x = pack_transfer(std::vector<Bytes>{raw});
    q.rx.insert(q.rx.end(), x.begin(), x.end());
}

// Head pose every 2 ms, yaw sweeping +-35 degrees over 4 s
void queue_pose(FakeQuest& q, int64_t now) {
    const double yaw = 35.0 * 3.14159265358979 / 180.0 * std::sin(2 * 3.14159265358979 * double(now - q.start_ns) / 4e9);
    const float quat[4] = {0.0f, float(std::sin(yaw / 2)), 0.0f, float(std::cos(yaw / 2))};
    Bytes table = q.pose_table, pose = q.pose;
    for (int i = 0; i < 4; ++i) {
        uint32_t u;
        std::memcpy(&u, &quat[i], 4);
        wr32(&pose[q.quat_at + 4 * size_t(i)], u);
    }
    wr16(&table[4], q.pose_seq++);
    wr16(&pose[4], q.pose_seq++);
    queue(q, table);
    queue(q, pose);
}

void measure(FakeQuest& q, const uint8_t* data, size_t len) {
    q.written.feed({data, len});
    Bytes raw;
    const int64_t now = now_ns();
    while (q.written.next(raw)) {
        const uint8_t topic = raw[1];
        ++q.messages;
        q.largest[topic] = std::max(q.largest[topic], raw.size());
        if (raw.size() > q.max_message) ++q.over_cap;
        if (topic >= 10 && topic < 15) {
            q.video_bytes += raw.size();
            q.second_bytes += raw.size();
        }
    }
    if (!q.window_since) q.window_since = q.second_since = now;
    if (now - q.second_since >= 1'000'000'000) {
        q.peak_second_bytes = std::max(q.peak_second_bytes, q.second_bytes * 1'000'000'000 / uint64_t(now - q.second_since));
        q.second_bytes = 0;
        q.second_since = now;
    }
    if (now - q.window_since >= 10'000'000'000 && q.log) {
        auto big = std::max_element(q.largest.begin(), q.largest.end(), [](auto& a, auto& b) { return a.second < b.second; });
        q.log("fake quest: video " + std::to_string(q.video_bytes * 8'000 / uint64_t(now - q.window_since)) + " Mbit/s avg, " +
              std::to_string(q.peak_second_bytes * 8 / 1'000'000) + " peak (1 s); largest message so far " +
              std::to_string(big == q.largest.end() ? 0 : big->second) + " B (topic " +
              std::to_string(big == q.largest.end() ? 0 : big->first) + "), " + std::to_string(q.over_cap) + " of " +
              std::to_string(q.messages) + " messages over the " + std::to_string(q.max_message) + " B cap");
        q.video_bytes = q.peak_second_bytes = 0;
        q.window_since = now;
    }
}

int fake_write(void* user, const uint8_t* data, size_t len, int) {
    auto& q = *static_cast<FakeQuest*>(user);
    std::lock_guard<std::mutex> l(q.mu);
    measure(q, data, len);
    return 0;
}

int fake_read(void* user, uint8_t* buf, size_t cap, int timeout_ms) {
    auto& q = *static_cast<FakeQuest*>(user);
    {
        std::lock_guard<std::mutex> l(q.mu);
        const int64_t now = now_ns();
        if (q.rx_pos == q.rx.size() && now - q.last_pose_ns >= 2'000'000) {
            q.rx.clear();
            q.rx_pos = 0;
            queue_pose(q, now);
            q.last_pose_ns = now;
        }
        size_t n = std::min(cap, q.rx.size() - q.rx_pos);
        if (n) {
            std::memcpy(buf, q.rx.data() + q.rx_pos, n);
            q.rx_pos += n;
            return int(n);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));
    return 0;
}

}  // namespace

std::shared_ptr<FakeQuest> load_fake_quest(const std::string& path, size_t max_message,
                                           std::function<void(const std::string&)> log, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *error = "cannot open " + path;
        return nullptr;
    }
    auto q = std::make_shared<FakeQuest>();
    std::vector<Bytes> templates;
    for (;;) {
        uint8_t head[5];
        if (!f.read(reinterpret_cast<char*>(head), 5)) break;
        Bytes raw(rd32(head + 1));
        if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) break;
        if (head[0] == 0)
            queue(*q, raw);
        else
            templates.push_back(std::move(raw));
    }
    if (templates.size() != 2 || templates[1].size() <= kHeaderSize) {
        *error = path + ": no head pose template";
        return nullptr;
    }
    q->pose_table = templates[0];
    q->pose = templates[1];
    auto segment = std::span<const uint8_t>(q->pose).subspan(kHeaderSize);
    auto head_pose = capnp::View::root(segment).ptr(1);
    if (head_pose.data().size() < 0x40) {
        *error = path + ": head pose template doesn't parse";
        return nullptr;
    }
    q->quat_at = kHeaderSize + size_t(head_pose.data().data() - segment.data()) + 0x30;  // pose.cpp kOrientation
    q->pose_seq = uint16_t(rd16(&q->pose[4]) + 1);
    q->start_ns = now_ns();
    q->max_message = max_message;
    q->log = std::move(log);
    return q;
}

qb_transport fake_quest_transport(const std::shared_ptr<FakeQuest>& quest) {
    qb_transport t{};
    t.user = quest.get();
    t.write = fake_write;
    t.read = fake_read;
    return t;
}

}  // namespace qb
