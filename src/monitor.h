// One monitor: Desktop Duplication capture, GPU pipeline, and the click-through overlay window.
#pragma once
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <string>
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
  float colorM[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // treated-area color recipe rows
  float colorCtl[4] = {0, 0, 0, 0};                         // time amount, enabled
};

struct MonitorStats {
  double fps = 0, frameMs = 0, inMean = 0, outMean = 0, minGain = 1, brightMean = 0;
};

class Monitor {
 public:
  Monitor(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDCompositionDevice* dcomp)
      : dev_(dev), ctx_(ctx), dcomp_(dcomp) {}
  ~Monitor();

  bool init(IDXGIOutput* output, int index, bool excludeFromCapture, std::string& err);

  // Grabs a new desktop image if one is available. Returns true on a new image.
  bool capture();
  // Duplication was lost (secure desktop, mode change...). Retry with reacquire().
  bool lost() const { return lost_; }
  bool reacquire();

  // Runs the pipeline and presents the overlay (skips work when idle).
  void process(const FrameInputs& in, bool newFrame);

  // Renders the preview (RGBA8, tightly packed) for the web UI.
  bool readPreview(const FrameInputs& in, int mode, std::vector<uint8_t>& rgba, int& w, int& h);
  bool readStats(const FrameInputs& in, MonitorStats& out);

  void reassertTopmost();
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

  ComPtr<IDXGIOutput1> output_;
  ComPtr<IDXGIOutputDuplication> dupl_;
  bool lost_ = false;
  bool hasFrame_ = false;

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
  ComPtr<ID3D11Texture2D> statsTex_, statsStaging_;
  ComPtr<ID3D11UnorderedAccessView> statsUAV_;

  ComPtr<ID3D11Buffer> frameCB_, passCB_;
  ComPtr<ID3D11SamplerState> sampler_;

  int lastKmin_ = -1, lastKmax_ = -1;
  bool processedOnce_ = false;
  bool lastBypass_ = false;
  std::chrono::steady_clock::time_point lastActivity_{};
  // fps accounting
  int frameCount_ = 0;
  double frameMsAccum_ = 0;
  std::chrono::steady_clock::time_point fpsWindow_ = std::chrono::steady_clock::now();
};

// Set by the overlay window procedure when the display configuration changes.
extern volatile bool g_displayChanged;
