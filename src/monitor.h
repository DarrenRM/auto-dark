// One monitor: Desktop Duplication capture, GPU pipeline, and the click-through overlay window.
#pragma once
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "shaders.h"

using Microsoft::WRL::ComPtr;

// Per-frame inputs shared by all monitors.
struct FrameInputs {
  const ShaderSet* shaders = nullptr;
  ID3D11Buffer* paramsCB = nullptr;
  std::vector<std::pair<double, double>> curve;
  int minLevel = 2, maxLevel = 13;
  float dtMs = 7;
  float timeSec = 0;
  bool bypass = false;
  bool resetTemporal = false;
  bool force = false;  // params changed: process even without a new desktop frame
  float settleMs = 1000;
  float minIntervalMs = 5;  // without a new frame, re-process at most this often (temporal easing)
  float colorM[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // treated-area color recipe rows
  float colorCtl[4] = {0, 0, 0, 0};                         // time amount, enabled
};

struct MonitorStats {
  double fps = 0, frameMs = 0, inMean = 0, outMean = 0, minGain = 1, brightMean = 0;
  // Latency, from Windows presenting a desktop frame to our overlay Present for it (ms):
  double latMs = 0, latMaxMs = 0;
  double acqMs = 0;  // of which: until the capture thread had the frame
};

class Monitor {
 public:
  Monitor(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDCompositionDevice* dcomp)
      : dev_(dev), ctx_(ctx), dcomp_(dcomp) {}
  ~Monitor();

  bool init(IDXGIOutput* output, int index, bool excludeFromCapture, std::string& err);

  // Starts the capture thread: it blocks on Desktop Duplication, copies each new desktop image and
  // signals `wake` immediately, so the overlay can be updated within the same refresh interval.
  void startCapture(HANDLE wake);
  void stopCapture();
  // Duplication currently unavailable (secure desktop, mode change...); the thread keeps retrying.
  bool lost() const { return lost_; }

  // Runs the pipeline on the newest captured image and presents the overlay. Skips work when idle.
  // Returns true if it presented (so the caller keeps a fast cadence while animating).
  bool process(const FrameInputs& in);

  // Preview (RGBA8, tightly packed) and stats for the web UI. Non-blocking: each call returns the
  // result requested by the previous call (if the GPU has finished it) and queues a new one, so the
  // render thread never stalls waiting for a readback.
  bool readPreview(const FrameInputs& in, int mode, std::vector<uint8_t>& rgba, int& w, int& h, int& resultMode);
  bool readStats(const FrameInputs& in, MonitorStats& out);

  void reassertTopmost();

  // "Start protected": up to 4 desktop-space rects that are dimmed to `gain` (linear) before their
  // content has been captured, e.g. a window that was just shown. gain 1 = inactive.
  void setPreDim(const RECT* rects, const float* gains, int count);

  // Test probe (--probe X Y): the capture thread samples one desktop pixel per captured frame.
  struct ProbeSample {
    int64_t qpc;
    uint8_t r, g, b;
  };
  void enableProbe(int x, int y);
  std::vector<ProbeSample> probeSamples();
  bool waitVBlank() { return output_ && SUCCEEDED(output_->WaitForVBlank()); }

  int index = 0;
  std::string name;
  int width = 0, height = 0;
  bool primary = false;
  bool captureExcluded = false;
  int numLevels = 1;
  RECT desktop{};
  MonitorStats stats;

  // log10 of the area fraction one cell of `level` covers.
  double levelLogArea(int level) const;
  int levelW(int level) const { return std::max(1, width >> level); }
  int levelH(int level) const { return std::max(1, height >> level); }

 private:
  bool createWindow(bool excludeFromCapture, std::string& err);
  bool createResources(std::string& err);
  bool ensureState(int level);
  void setFrameCB(const FrameInputs& in, int kmin, int kmax, int previewMode);
  void setPassCB(float a0, float a1, float a2, float a3, float b0, float b1, float b2);
  void runPipeline(const FrameInputs& in, int kmin, int kmax);
  void bindCommon(const FrameInputs& in);

  ID3D11Device* dev_;
  ID3D11DeviceContext* ctx_;
  IDCompositionDevice* dcomp_;

  void captureLoop();
  void sampleProbe(ID3D11Texture2D* desktopTex, int64_t qpc);
  float preDim_[4][4] = {};
  float preDimGain_[4] = {1, 1, 1, 1};
  bool probeOn_ = false;
  int probeX_ = 0, probeY_ = 0;
  ComPtr<ID3D11Texture2D> probeStaging_;
  std::mutex probeMu_;
  std::vector<ProbeSample> probe_;
  bool reacquire();
  bool createCaptureDevice(std::string& err);

  ComPtr<IDXGIOutput1> output_;
  ComPtr<IDXGIOutputDuplication> dupl_;  // owned by the capture thread
  std::atomic<bool> lost_{false};
  std::atomic<bool> hasFrame_{false};
  std::atomic<bool> newFrame_{false};
  std::atomic<bool> capRun_{false};
  std::thread capThread_;
  HANDLE wake_ = nullptr;
  // QPC timestamps of the newest captured frame: when Windows presented it, when we had it.
  std::atomic<int64_t> framePresentQpc_{0}, frameAcquireQpc_{0};
  // The capture thread has its own device (Desktop Duplication can block on its device), and hands
  // frames over through a shared texture guarded by a keyed mutex: key 0 = capture may write,
  // key 1 = a new frame is ready for the render thread. Neither side ever blocks the other.
  ComPtr<ID3D11Device> capDev_;
  ComPtr<ID3D11DeviceContext> capCtx_;
  ComPtr<ID3D11Texture2D> capShared_;      // capture-device view
  ComPtr<IDXGIKeyedMutex> capMutex_;
  ComPtr<ID3D11Texture2D> captureTex_;     // render-device view of the same texture
  ComPtr<IDXGIKeyedMutex> mainMutex_;

  HWND hwnd_ = nullptr;
  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> visual_;

  ComPtr<ID3D11Texture2D> frameTex_;
  ComPtr<ID3D11ShaderResourceView> frameSRV_;
  ComPtr<ID3D11Texture2D> lumaTex_;
  ComPtr<ID3D11ShaderResourceView> lumaSRV_;
  ComPtr<ID3D11UnorderedAccessView> lumaUAV_;
  ComPtr<ID3D11Texture2D> gainTex_;
  std::vector<ComPtr<ID3D11ShaderResourceView>> gainSRV_;
  std::vector<ComPtr<ID3D11UnorderedAccessView>> gainUAV_;
  ComPtr<ID3D11Texture2D> stateTex_;
  ComPtr<ID3D11ShaderResourceView> stateSRV_;
  ComPtr<ID3D11UnorderedAccessView> stateUAV_;
  int stateLevel_ = -1;
  bool stateFresh_ = true;

  ComPtr<ID3D11Texture2D> previewTex_, previewStaging_;
  ComPtr<ID3D11UnorderedAccessView> previewUAV_;
  int previewW_ = 0, previewH_ = 0;
  bool previewPending_ = false, statsPending_ = false;
  int previewPendingMode_ = 0;
  ComPtr<ID3D11Texture2D> statsTex_, statsStaging_;
  ComPtr<ID3D11UnorderedAccessView> statsUAV_;

  ComPtr<ID3D11Buffer> frameCB_, passCB_;
  ComPtr<ID3D11SamplerState> sampler_;

  int lastKmin_ = -1, lastKmax_ = -1;
  bool processedOnce_ = false;
  bool lastBypass_ = false;
  std::chrono::steady_clock::time_point lastActivity_{};
  std::chrono::steady_clock::time_point lastProcess_{};
  // fps accounting
  int frameCount_ = 0, latCount_ = 0;
  double frameMsAccum_ = 0, latAccum_ = 0, latMax_ = 0, acqAccum_ = 0;
  std::chrono::steady_clock::time_point fpsWindow_ = std::chrono::steady_clock::now();
};

// Set by the overlay window procedure when the display configuration changes, and by the capture
// path when its device or frame hand-over breaks: the main loop rebuilds all monitors.
extern volatile bool g_displayChanged;
// Set when the render device is removed/reset (overlay presents fail): the main loop recreates the
// whole GPU side.
extern volatile bool g_deviceLost;
