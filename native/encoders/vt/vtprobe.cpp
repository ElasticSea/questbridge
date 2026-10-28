// vtprobe: VideoToolbox throughput. S sessions of WxH, F frames submitted back to back with at most
// `inflight` frames per session in flight (optionally paced at hz); prints fps and frame latency.
// Usage: vtprobe S W H F inflight lowlatency [hz]   env: HEVC=1, HIGH=1 (Constrained High), Q=<0..1>
#include <VideoToolbox/VideoToolbox.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
using clk = std::chrono::steady_clock;
std::atomic<int> done{0}; std::vector<clk::time_point> sub, fin; std::atomic<int> inflight{0};
static void cb(void*, void* r, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef) { fin[(size_t)r] = clk::now(); done++; inflight--; }
int main(int argc, char** argv) {
  int S = atoi(argv[1]), W = atoi(argv[2]), H = atoi(argv[3]), F = atoi(argv[4]), maxin = atoi(argv[5]); int ll = atoi(argv[6]); double hz = argc > 7 ? atof(argv[7]) : 0;
  std::vector<VTCompressionSessionRef> ss(S);
  for (auto& s : ss) {
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (ll) CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanTrue);
    VTCompressionSessionCreate(nullptr, W, H, (getenv("HEVC") ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264), spec, nullptr, nullptr, cb, nullptr, &s);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
    if (!getenv("HEVC")) VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel, getenv("HIGH") ? kVTProfileLevel_H264_ConstrainedHigh_AutoLevel : kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel); if (getenv("Q")) { float q = atof(getenv("Q")); CFNumberRef qn = CFNumberCreate(nullptr, kCFNumberFloatType, &q); VTSessionSetProperty(s, kVTCompressionPropertyKey_Quality, qn); }
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    int br = 100000000 / S; CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &br);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AverageBitRate, n);
    VTCompressionSessionPrepareToEncodeFrames(s);
  }
  CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(ss[0]);
  std::vector<CVPixelBufferRef> bufs(8);
  for (int i = 0; i < 8; i++) { CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &bufs[i]); CVPixelBufferLockBaseAddress(bufs[i],0);
    auto* y=(uint8_t*)CVPixelBufferGetBaseAddressOfPlane(bufs[i],0); size_t st=CVPixelBufferGetBytesPerRowOfPlane(bufs[i],0);
    for (int r=0;r<H;r++) for(int c=0;c<W;c++) y[r*st+c]=(uint8_t)((c+r*3+i*40)^(c>>4)); 
    auto* uv=(uint8_t*)CVPixelBufferGetBaseAddressOfPlane(bufs[i],1); memset(uv,128,CVPixelBufferGetBytesPerRowOfPlane(bufs[i],1)*H/2);
    CVPixelBufferUnlockBaseAddress(bufs[i],0); }
  sub.resize(F * S); fin.resize(F * S);
  auto t0 = clk::now();
  for (int f = 0; f < F; f++) {
    if (hz > 0) std::this_thread::sleep_until(t0 + std::chrono::microseconds((long)(f * 1e6 / hz)));
    for (int s = 0; s < S; s++) {
      while (inflight >= maxin * S) std::this_thread::yield();
      size_t id = f * S + s; sub[id] = clk::now(); inflight++;
      VTCompressionSessionEncodeFrame(ss[s], bufs[f % 8], CMTimeMake(f, 72), CMTimeMake(1, 72), nullptr, (void*)id, nullptr);
    }
  }
  for (auto& s : ss) VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
  double total = std::chrono::duration<double>(clk::now() - t0).count();
  double lat = 0, mx = 0; for (int f = 10; f < F; f++) { double l = std::chrono::duration<double, std::milli>(fin[f*S+S-1] - sub[f*S]).count(); lat += l; if (l>mx) mx=l; }
  printf("S=%d %dx%d ll=%d inflight<=%d hz=%.0f: %.1f fps, frame latency mean %.2f max %.2f ms\n", S, W, H, ll, maxin, hz, F / total, lat / (F - 10), mx);
}
