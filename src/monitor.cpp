#include "monitor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"

volatile bool g_displayChanged = false;

namespace {

constexpr wchar_t kWindowClass[] = L"AutoDarkOverlay";
constexpr int kStatsW = 32, kStatsH = 18;
constexpr int kPreviewW = 640;

struct FrameCB {
  float screen[4];
  float stateSize[4];
  float time[4];
  float levels[4];
  float colorM[12];
  float colorCtl[4];
};
struct PassCB {
  float a[4];
  float b[4];
};

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DISPLAYCHANGE: g_displayChanged = true; break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClassOnce() {
  static bool done = false;
  if (done) return;
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = OverlayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWindowClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  done = true;
}

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

}  // namespace

Monitor::~Monitor() {
  if (dupl_) dupl_.Reset();
  visual_.Reset();
  target_.Reset();
  swap_.Reset();
  if (hwnd_) DestroyWindow(hwnd_);
}

double Monitor::levelLogArea(int level) const {
  return -std::log10((double)levelW(level) * (double)levelH(level));
}

bool Monitor::init(IDXGIOutput* output, int idx, bool excludeFromCapture, std::string& err) {
  index = idx;
  if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output_)))) { err = "IDXGIOutput1 unavailable"; return false; }
  DXGI_OUTPUT_DESC od{};
  output_->GetDesc(&od);
  name = narrow(od.DeviceName);
  desktop = od.DesktopCoordinates;
  width = desktop.right - desktop.left;
  height = desktop.bottom - desktop.top;
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(od.Monitor, &mi);
  primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  numLevels = (int)std::floor(std::log2((double)std::max(width, height))) + 1;
  if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
    logf("monitor %s: rotated outputs are not handled yet", name.c_str());

  if (!createResources(err)) return false;
  if (!createWindow(excludeFromCapture, err)) return false;
  if (!reacquire()) {
    // Not fatal: the secure desktop may be active. The main loop keeps retrying.
    logf("monitor %s: duplication not available yet", name.c_str());
  }
  logf("monitor %d: %s %dx%d at (%ld,%ld)%s, %d levels, capture-excluded=%d", index, name.c_str(), width, height,
       desktop.left, desktop.top, primary ? " primary" : "", numLevels, captureExcluded);
  return true;
}

bool Monitor::reacquire() {
  dupl_.Reset();
  HRESULT hr = output_->DuplicateOutput(dev_, &dupl_);
  lost_ = FAILED(hr);
  if (!lost_) hasFrame_ = false;
  return !lost_;
}

bool Monitor::createWindow(bool excludeFromCapture, std::string& err) {
  registerClassOnce();
  DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED |
             WS_EX_NOREDIRECTIONBITMAP;
  hwnd_ = CreateWindowExW(ex, kWindowClass, L"auto-dark overlay", WS_POPUP, desktop.left, desktop.top, width, height,
                          nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd_) { err = "CreateWindowEx failed"; return false; }
  // Layered + transparent = clicks pass through to whatever is underneath.
  SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
  captureExcluded = excludeFromCapture && SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
  if (excludeFromCapture && !captureExcluded)
    logf("monitor %s: SetWindowDisplayAffinity failed (%lu); overlay will feed back into capture", name.c_str(),
         GetLastError());

  ComPtr<IDXGIDevice> dxgiDev;
  dev_->QueryInterface(IID_PPV_ARGS(&dxgiDev));
  ComPtr<IDXGIAdapter> adapter;
  dxgiDev->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  adapter->GetParent(IID_PPV_ARGS(&factory));

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = width;
  sd.Height = height;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  sd.Scaling = DXGI_SCALING_STRETCH;
  HRESULT hr = factory->CreateSwapChainForComposition(dev_, &sd, nullptr, &swap_);
  if (FAILED(hr)) { err = "CreateSwapChainForComposition failed"; return false; }
  ComPtr<ID3D11Texture2D> back;
  swap_->GetBuffer(0, IID_PPV_ARGS(&back));
  dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);

  // Start fully transparent.
  float clear[4] = {0, 0, 0, 0};
  ctx_->ClearRenderTargetView(rtv_.Get(), clear);
  swap_->Present(0, 0);

  if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_)) || FAILED(dcomp_->CreateVisual(&visual_))) {
    err = "DirectComposition target/visual creation failed";
    return false;
  }
  visual_->SetContent(swap_.Get());
  target_->SetRoot(visual_.Get());
  dcomp_->Commit();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  return true;
}

bool Monitor::createResources(std::string& err) {
  auto fail = [&](const char* what) { err = what; return false; };

  D3D11_TEXTURE2D_DESC td{};
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &frameTex_))) return fail("frame texture");
  dev_->CreateShaderResourceView(frameTex_.Get(), nullptr, &frameSRV_);

  td.MipLevels = numLevels;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &lumaTex_))) return fail("luma pyramid");
  dev_->CreateShaderResourceView(lumaTex_.Get(), nullptr, &lumaSRV_);
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = td.Format;
  ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  ud.Texture2D.MipSlice = 0;
  dev_->CreateUnorderedAccessView(lumaTex_.Get(), &ud, &lumaUAV_);

  td.Format = DXGI_FORMAT_R16_FLOAT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  td.MiscFlags = 0;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &gainTex_))) return fail("gain pyramid");
  gainSRV_.resize(numLevels);
  gainUAV_.resize(numLevels);
  for (int k = 0; k < numLevels; k++) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = k;
    sd.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(gainTex_.Get(), &sd, &gainSRV_[k]);
    ud.Format = td.Format;
    ud.Texture2D.MipSlice = k;
    dev_->CreateUnorderedAccessView(gainTex_.Get(), &ud, &gainUAV_[k]);
  }

  previewW_ = std::min(kPreviewW, width);
  previewH_ = std::max(1, (int)std::lround((double)previewW_ * height / width));
  td = {};
  td.Width = previewW_;
  td.Height = previewH_;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &previewTex_))) return fail("preview texture");
  dev_->CreateUnorderedAccessView(previewTex_.Get(), nullptr, &previewUAV_);
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev_->CreateTexture2D(&td, nullptr, &previewStaging_);

  td = {};
  td.Width = kStatsW;
  td.Height = kStatsH;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &statsTex_))) return fail("stats texture");
  dev_->CreateUnorderedAccessView(statsTex_.Get(), nullptr, &statsUAV_);
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev_->CreateTexture2D(&td, nullptr, &statsStaging_);

  D3D11_BUFFER_DESC bd{};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.ByteWidth = sizeof(FrameCB);
  dev_->CreateBuffer(&bd, nullptr, &frameCB_);
  bd.ByteWidth = sizeof(PassCB);
  dev_->CreateBuffer(&bd, nullptr, &passCB_);

  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  dev_->CreateSamplerState(&smp, &sampler_);
  return true;
}

bool Monitor::ensureState(int level) {
  if (stateLevel_ == level && stateTex_) return true;
  stateTex_.Reset();
  stateSRV_.Reset();
  stateUAV_.Reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = levelW(level);
  td.Height = levelH(level);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32_FLOAT;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &stateTex_))) return false;
  dev_->CreateShaderResourceView(stateTex_.Get(), nullptr, &stateSRV_);
  dev_->CreateUnorderedAccessView(stateTex_.Get(), nullptr, &stateUAV_);
  stateLevel_ = level;
  stateFresh_ = true;
  return true;
}

bool Monitor::capture() {
  if (!dupl_ || lost_) return false;
  DXGI_OUTDUPL_FRAME_INFO fi{};
  ComPtr<IDXGIResource> res;
  HRESULT hr = dupl_->AcquireNextFrame(0, &fi, &res);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
  if (FAILED(hr)) {
    lost_ = true;
    return false;
  }
  bool got = false;
  if (fi.LastPresentTime.QuadPart != 0 || !hasFrame_) {
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
      D3D11_TEXTURE2D_DESC d{};
      tex->GetDesc(&d);
      if ((int)d.Width == width && (int)d.Height == height && d.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        ctx_->CopyResource(frameTex_.Get(), tex.Get());
        got = true;
        hasFrame_ = true;
      } else {
        g_displayChanged = true;  // size/format mismatch: rebuild
      }
    }
  }
  dupl_->ReleaseFrame();
  return got;
}

void Monitor::setFrameCB(const FrameInputs& in, int kmin, int kmax, int previewMode) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(frameCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  FrameCB cb{};
  cb.screen[0] = (float)width;
  cb.screen[1] = (float)height;
  cb.screen[2] = 1.0f / width;
  cb.screen[3] = 1.0f / height;
  int sw = levelW(stateLevel_ < 0 ? kmin : stateLevel_), sh = levelH(stateLevel_ < 0 ? kmin : stateLevel_);
  cb.stateSize[0] = (float)sw;
  cb.stateSize[1] = (float)sh;
  cb.stateSize[2] = 1.0f / sw;
  cb.stateSize[3] = 1.0f / sh;
  cb.time[0] = in.dtMs;
  cb.time[1] = in.timeSec;
  cb.time[2] = (in.resetTemporal || stateFresh_) ? 1.0f : 0.0f;
  cb.time[3] = in.bypass ? 1.0f : 0.0f;
  cb.levels[0] = (float)kmin;
  cb.levels[1] = (float)kmax;
  cb.levels[2] = (float)numLevels;
  cb.levels[3] = (float)previewMode;
  memcpy(cb.colorM, in.colorM, sizeof cb.colorM);
  memcpy(cb.colorCtl, in.colorCtl, sizeof cb.colorCtl);
  memcpy(m.pData, &cb, sizeof cb);
  ctx_->Unmap(frameCB_.Get(), 0);
}

void Monitor::setPassCB(float a0, float a1, float a2, float a3, float b0, float b1, float b2) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(passCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  PassCB cb{{a0, a1, a2, a3}, {b0, b1, b2, 0}};
  memcpy(m.pData, &cb, sizeof cb);
  ctx_->Unmap(passCB_.Get(), 0);
}

void Monitor::bindCommon(const FrameInputs& in) {
  ID3D11Buffer* cbs[3] = {frameCB_.Get(), in.paramsCB, passCB_.Get()};
  ctx_->CSSetConstantBuffers(0, 3, cbs);
  ctx_->PSSetConstantBuffers(0, 3, cbs);
  ctx_->VSSetConstantBuffers(0, 3, cbs);
  ID3D11SamplerState* s = sampler_.Get();
  ctx_->CSSetSamplers(0, 1, &s);
  ctx_->PSSetSamplers(0, 1, &s);
}

static void unbindCS(ID3D11DeviceContext* ctx) {
  ID3D11ShaderResourceView* nullSRV[3] = {};
  ID3D11UnorderedAccessView* nullUAV[1] = {};
  ctx->CSSetShaderResources(0, 3, nullSRV);
  ctx->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
}

static UINT groups(int n, int size) { return (UINT)((n + size - 1) / size); }

void Monitor::runPipeline(const FrameInputs& in, int kmin, int kmax) {
  const ShaderSet& sh = *in.shaders;
  ensureState(kmin);
  setFrameCB(in, kmin, kmax, 0);
  bindCommon(in);

  // 1. luminance base + pyramid
  ctx_->CSSetShader(sh.luma.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv = frameSRV_.Get();
  ctx_->CSSetShaderResources(0, 1, &srv);
  ID3D11UnorderedAccessView* uav = lumaUAV_.Get();
  ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx_->Dispatch(groups(width, 16), groups(height, 16), 1);
  unbindCS(ctx_);
  ctx_->GenerateMips(lumaSRV_.Get());

  // 2. budgets, coarse to fine
  ctx_->CSSetShader(sh.gain.Get(), nullptr, 0);
  for (int k = kmax; k >= kmin; k--) {
    double budget = ParamStore::evalCurve(in.curve, levelLogArea(k));
    bool top = k == kmax;
    int cw = top ? 1 : levelW(k + 1), chh = top ? 1 : levelH(k + 1);
    setPassCB((float)k, top ? 1.0f : 0.0f, (float)levelW(k), (float)levelH(k), (float)cw, (float)chh, (float)budget);
    ID3D11ShaderResourceView* srvs[2] = {lumaSRV_.Get(), top ? nullptr : gainSRV_[k + 1].Get()};
    ctx_->CSSetShaderResources(0, 2, srvs);
    uav = gainUAV_[k].Get();
    ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx_->Dispatch(groups(levelW(k), 8), groups(levelH(k), 8), 1);
    unbindCS(ctx_);
  }

  // 3. temporal
  ctx_->CSSetShader(sh.temporal.Get(), nullptr, 0);
  srv = gainSRV_[kmin].Get();
  ctx_->CSSetShaderResources(0, 1, &srv);
  uav = stateUAV_.Get();
  ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx_->Dispatch(groups(levelW(kmin), 8), groups(levelH(kmin), 8), 1);
  unbindCS(ctx_);
  stateFresh_ = false;
}

void Monitor::process(const FrameInputs& in, bool newFrame) {
  if (!in.shaders || !hasFrame_) return;
  auto now = std::chrono::steady_clock::now();
  double win = std::chrono::duration<double>(now - fpsWindow_).count();
  if (win >= 1.0) {
    stats.fps = frameCount_ / win;
    stats.frameMs = frameCount_ ? frameMsAccum_ / frameCount_ : 0;
    frameCount_ = 0;
    frameMsAccum_ = 0;
    fpsWindow_ = now;
  }
  if (newFrame || in.force || in.bypass != lastBypass_ || !processedOnce_) lastActivity_ = now;
  // Idle: nothing changed and the temporal state has had time to settle.
  double idleMs = std::chrono::duration<double, std::milli>(now - lastActivity_).count();
  if (idleMs > in.settleMs) return;

  int kmax = std::clamp(in.maxLevel, 0, numLevels - 1);
  int kmin = std::clamp(in.minLevel, 0, kmax);
  lastKmin_ = kmin;
  lastKmax_ = kmax;

  runPipeline(in, kmin, kmax);

  // 4. overlay
  const ShaderSet& sh = *in.shaders;
  setFrameCB(in, kmin, kmax, 0);
  D3D11_VIEWPORT vp{0, 0, (float)width, (float)height, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ID3D11RenderTargetView* rtv = rtv_.Get();
  ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(sh.vs.Get(), nullptr, 0);
  ctx_->PSSetShader(sh.ps.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[3] = {frameSRV_.Get(), stateSRV_.Get(), lumaSRV_.Get()};
  ctx_->PSSetShaderResources(0, 3, srvs);
  ctx_->Draw(3, 0);
  ID3D11ShaderResourceView* nullSRV[3] = {};
  ctx_->PSSetShaderResources(0, 3, nullSRV);
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);

  swap_->Present(0, 0);
  processedOnce_ = true;
  lastBypass_ = in.bypass;

  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
  frameCount_++;
  frameMsAccum_ += ms;
}

bool Monitor::readPreview(const FrameInputs& in, int mode, std::vector<uint8_t>& rgba, int& w, int& h) {
  if (!in.shaders || !processedOnce_ || !stateTex_) return false;
  setFrameCB(in, lastKmin_, lastKmax_, mode);
  bindCommon(in);
  ctx_->CSSetShader(in.shaders->preview.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[3] = {frameSRV_.Get(), stateSRV_.Get(), lumaSRV_.Get()};
  ctx_->CSSetShaderResources(0, 3, srvs);
  ID3D11UnorderedAccessView* uav = previewUAV_.Get();
  ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx_->Dispatch(groups(previewW_, 8), groups(previewH_, 8), 1);
  unbindCS(ctx_);
  ctx_->CopyResource(previewStaging_.Get(), previewTex_.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(previewStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  w = previewW_;
  h = previewH_;
  rgba.resize((size_t)w * h * 4);
  for (int y = 0; y < h; y++) memcpy(&rgba[(size_t)y * w * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
  ctx_->Unmap(previewStaging_.Get(), 0);
  return true;
}

bool Monitor::readStats(const FrameInputs& in, MonitorStats& out) {
  out = stats;
  if (!in.shaders || !processedOnce_ || !stateTex_) return false;
  setFrameCB(in, lastKmin_, lastKmax_, 0);
  int level = std::clamp((int)std::floor(std::log2((double)width / kStatsW)), 0, numLevels - 1);
  setPassCB((float)level, 0, 0, 0, 0, 0, 0);
  bindCommon(in);
  ctx_->CSSetShader(in.shaders->stats.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[3] = {nullptr, stateSRV_.Get(), lumaSRV_.Get()};
  ctx_->CSSetShaderResources(0, 3, srvs);
  ID3D11UnorderedAccessView* uav = statsUAV_.Get();
  ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx_->Dispatch(groups(kStatsW, 8), groups(kStatsH, 8), 1);
  unbindCS(ctx_);
  ctx_->CopyResource(statsStaging_.Get(), statsTex_.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(statsStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  double sumIn = 0, sumOut = 0, sumE = 0, minG = 1;
  for (int y = 0; y < kStatsH; y++) {
    const float* row = (const float*)((uint8_t*)m.pData + (size_t)y * m.RowPitch);
    for (int x = 0; x < kStatsW; x++) {
      sumIn += row[x * 4 + 0];
      sumOut += row[x * 4 + 1];
      minG = std::min(minG, (double)row[x * 4 + 2]);
      sumE += row[x * 4 + 3];
    }
  }
  ctx_->Unmap(statsStaging_.Get(), 0);
  double n = kStatsW * kStatsH;
  stats.inMean = sumIn / n;
  stats.outMean = sumOut / n;
  stats.brightMean = sumE / n;
  stats.minGain = minG;
  out = stats;
  return true;
}

void Monitor::reassertTopmost() {
  if (hwnd_)
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
