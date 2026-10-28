#include "strips.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ql::enc {

namespace {

// Runs fn(0..n-1) in parallel: index 0 on the caller's thread, the others on persistent workers.
class Pool {
public:
    explicit Pool(int n) : n_(n) {
        for (int i = 1; i < n; i++) threads_.emplace_back([this, i] { loop(i); });
    }

    ~Pool() {
        {
            std::lock_guard l(m_);
            stop_ = true;
            gen_++;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

    void run(const std::function<void(int)>& fn) {
        {
            std::lock_guard l(m_);
            fn_ = &fn;
            pending_ = n_ - 1;
            gen_++;
        }
        cv_.notify_all();
        fn(0);
        std::unique_lock l(m_);
        done_.wait(l, [&] { return pending_ == 0; });
    }

private:
    void loop(int i) {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)>* fn;
            {
                std::unique_lock l(m_);
                cv_.wait(l, [&] { return gen_ != seen; });
                seen = gen_;
                if (stop_) return;
                fn = fn_;
            }
            (*fn)(i);
            std::lock_guard l(m_);
            if (--pending_ == 0) done_.notify_one();
        }
    }

    int n_;
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)>* fn_ = nullptr;
    uint64_t gen_ = 0;
    int pending_ = 0;
    bool stop_ = false;
};

class StripEncoder final : public Encoder {
public:
    StripEncoder(const EncoderConfig& c, GpuInput gpu) : cfg_(c), gpu_(std::move(gpu)) {}

    bool init(const StripFactory& factory, std::string* err) {
        const int n = std::max(cfg_.slices, 1);
        strip_h_ = cfg_.height / n;
        if (strip_h_ % 16 && n > 1) return fail(err, "strip height " + std::to_string(strip_h_) + " is not whole MB rows");
        EncoderConfig sc = cfg_;
        sc.height = strip_h_;
        sc.slices = 1;
        sc.layout = Layout::Slices;
        sc.bitrate = cfg_.bitrate / uint32_t(n);
        for (int i = 0; i < n; i++) {
            std::string e;
            auto enc = factory(sc, i, &e);
            if (!enc) return fail(err, "strip " + std::to_string(i) + ": " + e);
            subs_.push_back(std::move(enc));
        }
        outs_.resize(size_t(n));
        ok_.resize(size_t(n));
        pool_ = std::make_unique<Pool>(n);
        name_ = subs_[0]->name() + ", " + std::to_string(n) + " strips of " + std::to_string(cfg_.width) + "x" +
                std::to_string(strip_h_);
        return true;
    }

    bool encode(const CpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        auto t0 = std::chrono::steady_clock::now();
        std::function<void(int)> job = [&](int i) {
            CpuFrame s = f;
            const size_t row = size_t(i) * strip_h_;
            s.plane[0] = f.plane[0] + row * size_t(f.stride[0]);
            if (f.format == PixelFormat::NV12) s.plane[1] = f.plane[1] + row / 2 * size_t(f.stride[1]);
            ok_[size_t(i)] = subs_[size_t(i)]->encode(s, ts, force_idr, outs_[size_t(i)]);
        };
        pool_->run(job);
        return merge(ts, t0, out);
    }

    bool encode(const GpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        std::string e;
        if (gpu_.split) {
            auto t0 = std::chrono::steady_clock::now();
            if (!gpu_.split(f, gpu_strips_, &e)) return fail(nullptr, e);
            if (gpu_strips_.size() != subs_.size()) return fail(nullptr, "GPU split gave the wrong strip count");
            std::function<void(int)> job = [&](int i) {
                ok_[size_t(i)] = subs_[size_t(i)]->encode(gpu_strips_[size_t(i)], ts, force_idr, outs_[size_t(i)]);
            };
            pool_->run(job);
            return merge(ts, t0, out);
        }
        if (gpu_.read_back) {
            CpuFrame c;
            if (!gpu_.read_back(f, c, &e)) return fail(nullptr, e);
            return encode(c, ts, force_idr, out);
        }
        return fail(nullptr, "GpuFrame input not supported by " + name_);
    }

    std::string name() const override { return name_; }
    const std::string& error() const override { return error_; }
    Layout layout() const override { return Layout::Strips; }

private:
    bool fail(std::string* err, std::string msg) {
        error_ = std::move(msg);
        if (err) *err = error_;
        return false;
    }

    bool merge(int64_t ts, std::chrono::steady_clock::time_point t0, EncodedFrame& out) {
        out = EncodedFrame{};
        out.timestamp_ns = ts;
        out.idr = true;
        for (size_t i = 0; i < subs_.size(); i++) {
            EncodedFrame& o = outs_[i];
            if (!ok_[i]) return fail(nullptr, "strip " + std::to_string(i) + ": " + subs_[i]->error());
            if (o.slices.size() != 1)
                return fail(nullptr, "strip " + std::to_string(i) + ": " + std::to_string(o.slices.size()) +
                                         " slices, want 1");
            Slice s = std::move(o.slices[0]);
            s.index = int(i);
            s.csd = o.csd;
            out.slices.push_back(std::move(s));
            out.idr = out.idr && o.idr;  // isIntraFrame only when every strip is (a re-encode is one strip)
            out.refits += o.refits;
            out.first_try_bytes = std::max(out.first_try_bytes, o.first_try_bytes);
        }
        out.csd = outs_[0].csd;
        out.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return true;
    }

    EncoderConfig cfg_;
    GpuInput gpu_;
    int strip_h_ = 0;
    std::vector<std::unique_ptr<Encoder>> subs_;
    std::vector<EncodedFrame> outs_;
    std::vector<char> ok_;
    std::vector<GpuFrame> gpu_strips_;
    std::unique_ptr<Pool> pool_;
    std::string name_, error_;
};

}  // namespace

std::unique_ptr<Encoder> make_strip_encoder(const EncoderConfig& config, const StripFactory& factory, GpuInput gpu,
                                            std::string* error) {
    auto enc = std::make_unique<StripEncoder>(config, std::move(gpu));
    if (!enc->init(factory, error)) return nullptr;
    return enc;
}

}  // namespace ql::enc
