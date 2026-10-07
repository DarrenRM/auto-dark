// auto-dark: always-on spatial luminance limiter ("flashbang guard").
// Backend A: Desktop Duplication capture -> GPU budget pipeline -> click-through dimming overlay.
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "color.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "monitor.h"
#include "params.h"
#include "shaders.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

bool encodeJpeg(const std::vector<uint8_t>& rgba, int w, int h, float quality, std::vector<uint8_t>& out);

namespace {

enum HotkeyId { kHotBypass = 1, kHotProfile = 2, kHotDebug = 3 };

// State shared between the render thread and the HTTP thread.
struct Shared {
  std::mutex mu;
  json::Value monitors = json::Array{};
  json::Value stats = json::Object{};
  std::string shaderError;
  bool captureExcluded = true;
  std::atomic<bool> bypass{false};
  std::atomic<bool> resetTemporal{false};
  std::atomic<bool> quit{false};
  // preview: requested by the UI, rendered by the render thread
  int previewMon = 0, previewMode = 0;
  std::chrono::steady_clock::time_point previewRequested{};
  std::string previewJpeg;
  int previewJpegMon = -1, previewJpegMode = -1;
};

Shared g;

BOOL WINAPI ConsoleHandler(DWORD) {
  g.quit = true;
  return TRUE;
}

std::string findRoot() {
  // Prefer the source tree (hot reload of shaders/web); fall back to the exe directory.
  if (fs::exists(fs::u8path(std::string(AUTO_DARK_ROOT) + "/shaders/params.json"))) return AUTO_DARK_ROOT;
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  fs::path p = fs::path(buf).parent_path();
  for (int i = 0; i < 4; i++) {
    if (fs::exists(p / "shaders" / "params.json")) return p.u8string();
    p = p.parent_path();
  }
  return AUTO_DARK_ROOT;
}

HttpResponse jsonResponse(const json::Value& v, int status = 200) {
  HttpResponse r;
  r.status = status;
  r.body = json::dump(v);
  return r;
}

HttpResponse okOrError(bool ok, const std::string& err) {
  if (ok) return jsonResponse(json::Object{{"ok", true}});
  return jsonResponse(json::Object{{"ok", false}, {"error", err}}, 400);
}

HttpResponse serveStatic(const std::string& root, std::string path) {
  if (path == "/") path = "/index.html";
  HttpResponse r;
  if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
    r.status = 404;
    return r;
  }
  bool ok = false;
  r.body = readFile(root + "/web" + path, &ok);
  if (!ok) {
    r.status = 404;
    r.contentType = "text/plain";
    r.body = "not found";
    return r;
  }
  std::string ext = fs::path(path).extension().string();
  r.contentType = ext == ".html" ? "text/html; charset=utf-8"
                  : ext == ".js"  ? "text/javascript; charset=utf-8"
                  : ext == ".css" ? "text/css; charset=utf-8"
                  : ext == ".svg" ? "image/svg+xml"
                  : ext == ".png" ? "image/png"
                  : ext == ".ico" ? "image/x-icon"
                  : ext == ".json" ? "application/json"
                                   : "application/octet-stream";
  return r;
}

HttpResponse handleApi(const std::string& root, ParamStore& params, const HttpRequest& req) {
  const std::string& p = req.path;
  json::Value body;
  if (req.method == "POST") {
    try {
      body = req.body.empty() ? json::Value(json::Object{}) : json::parse(req.body);
    } catch (std::exception& e) {
      return jsonResponse(json::Object{{"ok", false}, {"error", e.what()}}, 400);
    }
  }

  if (req.method == "GET" && p == "/api/schema") return jsonResponse(params.schema());

  if (req.method == "GET" && p == "/api/state") {
    std::lock_guard lk(g.mu);
    return jsonResponse(json::Object{
        {"profile", params.activeProfile()},
        {"dirty", params.dirty()},
        {"bypass", g.bypass.load()},
        {"schemaVersion", params.schemaVersion()},
        {"params", params.values()},
        {"monitors", g.monitors.clone()},
        {"shaderError", g.shaderError.empty() ? json::Value() : json::Value(g.shaderError)},
        {"captureExcluded", g.captureExcluded},
    });
  }

  if (req.method == "GET" && p == "/api/stats") {
    std::lock_guard lk(g.mu);
    json::Value s = g.stats.clone();
    if (s.isObj()) {
      (*s.o)["bypass"] = g.bypass.load();
      (*s.o)["shaderError"] = g.shaderError.empty() ? json::Value() : json::Value(g.shaderError);
    }
    return jsonResponse(s);
  }

  if (req.method == "GET" && p == "/api/preview") {
    int mon = req.query.count("mon") ? atoi(req.query.at("mon").c_str()) : 0;
    int mode = req.query.count("mode") ? atoi(req.query.at("mode").c_str()) : 0;
    std::lock_guard lk(g.mu);
    g.previewMon = mon;
    g.previewMode = mode;
    g.previewRequested = std::chrono::steady_clock::now();
    HttpResponse r;
    if (g.previewJpeg.empty() || g.previewJpegMon != mon || g.previewJpegMode != mode) {
      r.status = 204;
      r.contentType = "text/plain";
      return r;
    }
    r.contentType = "image/jpeg";
    r.body = g.previewJpeg;
    return r;
  }

  if (req.method == "POST" && p == "/api/params") {
    if (!body.isObj()) return okOrError(false, "expected an object");
    params.set(body);
    return okOrError(true, "");
  }

  if (req.method == "GET" && p == "/api/profiles") {
    json::Array names;
    for (auto& n : params.listProfiles()) names.push_back(n);
    return jsonResponse(json::Object{{"profiles", names}, {"active", params.activeProfile()}});
  }

  std::string err;
  if (req.method == "POST" && p == "/api/profile/load") {
    bool ok = params.loadProfile(body["name"].str(), err);
    if (ok) g.resetTemporal = true;
    return okOrError(ok, err);
  }
  if (req.method == "POST" && p == "/api/profile/save") return okOrError(params.saveProfile(body["name"].str(), err), err);
  if (req.method == "POST" && p == "/api/profile/delete") return okOrError(params.deleteProfile(body["name"].str(), err), err);

  if (req.method == "POST" && p == "/api/control") {
    if (body.has("bypass")) {
      g.bypass = body["bypass"].num() != 0;
      logf("bypass %s (web)", g.bypass ? "ON" : "off");
    }
    if (body.has("resetTemporal")) g.resetTemporal = true;
    if (body.has("quit") && body["quit"].num() != 0) g.quit = true;
    return okOrError(true, "");
  }

  return jsonResponse(json::Object{{"error", "not found"}}, 404);
}

struct Args {
  int port = 7777;
  bool bypass = false;
  bool open = false;
  bool exclude = true;
  bool debugLayer = false;
  std::string profile;
};

Args parseArgs() {
  Args a;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++) {
    std::wstring s = argv[i];
    if (s == L"--port" && i + 1 < argc) a.port = _wtoi(argv[++i]);
    else if (s == L"--bypass") a.bypass = true;
    else if (s == L"--open") a.open = true;
    else if (s == L"--no-exclude") a.exclude = false;
    else if (s == L"--debug-layer") a.debugLayer = true;
    else if (s == L"--profile" && i + 1 < argc) a.profile = fs::path(argv[++i]).u8string();
  }
  LocalFree(argv);
  return a;
}

struct Gpu {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDCompositionDevice> dcomp;
};

bool createGpu(bool debugLayer, Gpu& gpu) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  // Use the adapter that drives the monitors (Desktop Duplication requires it).
  for (UINT i = 0;; i++) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    ComPtr<IDXGIOutput> o;
    if (SUCCEEDED(a->EnumOutputs(0, &o))) {
      gpu.adapter = a;
      break;
    }
  }
  if (!gpu.adapter) return false;
  DXGI_ADAPTER_DESC1 ad{};
  gpu.adapter->GetDesc1(&ad);
  char name[128];
  WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
  logf("gpu: %s", name);

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0);
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                               D3D11_SDK_VERSION, &gpu.dev, nullptr, &gpu.ctx)))
    return false;
  ComPtr<IDXGIDevice1> dxgiDev;
  gpu.dev.As(&dxgiDev);
  dxgiDev->SetMaximumFrameLatency(1);
  return SUCCEEDED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&gpu.dcomp)));
}

std::vector<std::unique_ptr<Monitor>> buildMonitors(Gpu& gpu, bool exclude) {
  std::vector<std::unique_ptr<Monitor>> mons;
  for (UINT i = 0;; i++) {
    ComPtr<IDXGIOutput> o;
    if (gpu.adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_OUTPUT_DESC od{};
    o->GetDesc(&od);
    if (!od.AttachedToDesktop) continue;
    auto m = std::make_unique<Monitor>(gpu.dev.Get(), gpu.ctx.Get(), gpu.dcomp.Get());
    std::string err;
    if (m->init(o.Get(), (int)mons.size(), exclude, err)) mons.push_back(std::move(m));
    else logf("monitor %u: %s", i, err.c_str());
  }
  // Primary first, so index 0 is the main monitor in the UI.
  std::stable_sort(mons.begin(), mons.end(), [](auto& a, auto& b) { return a->primary > b->primary; });
  for (size_t i = 0; i < mons.size(); i++) mons[i]->index = (int)i;
  return mons;
}

void publishMonitors(const std::vector<std::unique_ptr<Monitor>>& mons) {
  json::Array arr;
  bool excluded = true;
  for (auto& m : mons) {
    json::Array levels;
    for (int k = 0; k < m->numLevels; k++)
      levels.push_back(json::Object{{"level", k}, {"logArea", m->levelLogArea(k)},
                                    {"cellWidth", m->width / m->levelW(k)}, {"cellHeight", m->height / m->levelH(k)}});
    arr.push_back(json::Object{{"index", m->index}, {"name", m->name}, {"width", m->width}, {"height", m->height},
                               {"primary", m->primary}, {"levels", levels}});
    excluded &= m->captureExcluded;
  }
  std::lock_guard lk(g.mu);
  g.monitors = arr;
  g.captureExcluded = excluded;
}

}  // namespace

int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  timeBeginPeriod(1);

  Args args = parseArgs();
  std::string root = findRoot();
  logf("auto-dark starting (root %s)", root.c_str());

  ParamStore params(root);
  params.init(args.profile);
  g.bypass = args.bypass;

  Gpu gpu;
  if (!createGpu(args.debugLayer, gpu)) {
    logf("fatal: could not create the D3D11 device");
    return 1;
  }

  ShaderManager shaders(root);
  {
    std::string err;
    shaders.sourcesChanged(nullptr);  // record initial timestamps
    if (!shaders.compile(gpu.dev.Get(), err)) logf("shaders: %s", err.c_str());
    else logf("shaders: compiled");
    std::lock_guard lk(g.mu);
    g.shaderError = err;
  }

  auto monitors = buildMonitors(gpu, args.exclude);
  publishMonitors(monitors);
  if (!args.exclude) logf("WARNING: --no-exclude: the overlay is visible to capture (debug only, feedback expected)");

  RegisterHotKey(nullptr, kHotBypass, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12);
  RegisterHotKey(nullptr, kHotProfile, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11);
  RegisterHotKey(nullptr, kHotDebug, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
  logf("hotkeys: Ctrl+Alt+F12 bypass, Ctrl+Alt+F11 next profile, Ctrl+Alt+F10 debug view");

  HttpServer http;
  http.start(args.port, [&](const HttpRequest& req) {
    if (req.path.rfind("/api/", 0) == 0) return handleApi(root, params, req);
    if (req.method != "GET") return jsonResponse(json::Object{{"error", "not found"}}, 404);
    return serveStatic(root, req.path);
  });
  if (args.open) ShellExecuteW(nullptr, L"open", (L"http://127.0.0.1:" + std::to_wstring(args.port) + L"/").c_str(),
                               nullptr, nullptr, SW_SHOWNORMAL);

  ColorPlan color;
  float lastColor[16] = {};

  ComPtr<ID3D11Buffer> paramsCB;
  uint32_t paramsCBSize = 0;
  uint64_t paramsVersion = 0;
  const ShaderSet* paramsShaderSet = nullptr;

  using clock = std::chrono::steady_clock;
  auto start = clock::now(), last = start, lastPoll = start, lastStats = start, lastPreview = start,
       lastTopmost = start, lastRetry = start;

  while (!g.quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) g.quit = true;
      if (msg.message == WM_HOTKEY) {
        if (msg.wParam == kHotBypass) {
          g.bypass = !g.bypass;
          logf("bypass %s (hotkey)", g.bypass ? "ON" : "off");
        } else if (msg.wParam == kHotProfile) {
          if (params.cycleProfile()) g.resetTemporal = true;
        } else if (msg.wParam == kHotDebug) {
          params.cycleEnum("debugView");
        }
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    auto now = clock::now();

    if (g_displayChanged) {
      g_displayChanged = false;
      logf("display configuration changed; rebuilding monitors");
      monitors.clear();
      Sleep(500);
      monitors = buildMonitors(gpu, args.exclude);
      publishMonitors(monitors);
    }

    // Hot reload of shaders and params.json.
    if (now - lastPoll > std::chrono::milliseconds(300)) {
      lastPoll = now;
      bool paramsJson = false;
      if (shaders.sourcesChanged(&paramsJson)) {
        std::string err;
        if (paramsJson && !params.reloadSchema(err)) logf("params: %s", err.c_str());
        if (err.empty()) {
          if (shaders.compile(gpu.dev.Get(), err)) logf("shaders: reloaded");
          else logf("shaders: %s", err.c_str());
        }
        std::lock_guard lk(g.mu);
        g.shaderError = err;
      }
    }

    auto set = shaders.current();
    float dtMs = (float)std::chrono::duration<double, std::milli>(now - last).count();
    last = now;

    // Params cbuffer (rebuilt when values or the reflected layout change).
    bool force = false;
    uint64_t v = params.version();
    if (set && (v != paramsVersion || set.get() != paramsShaderSet)) {
      force = true;
      paramsVersion = v;
      paramsShaderSet = set.get();
      auto bytes = set->packParams(params);
      if (!paramsCB || paramsCBSize != bytes.size()) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)bytes.size();
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        paramsCB.Reset();
        gpu.dev->CreateBuffer(&bd, nullptr, &paramsCB);
        paramsCBSize = (uint32_t)bytes.size();
      }
      gpu.ctx->UpdateSubresource(paramsCB.Get(), 0, nullptr, bytes.data(), 0, 0);
    }

    // Treated-area color recipe + time-of-day amount (applied per pixel by the overlay shader).
    color.update(params, g.bypass);

    FrameInputs in;
    in.shaders = set.get();
    in.paramsCB = paramsCB.Get();
    in.curve = params.curve();
    in.minLevel = (int)params.get("minLevel", 2);
    in.maxLevel = (int)params.get("maxLevel", 13);
    in.dtMs = std::min(dtMs, 50.0f);  // after idle gaps, don't jump the temporal state
    in.timeSec = (float)std::chrono::duration<double>(now - start).count();
    in.bypass = g.bypass;
    memcpy(in.colorM, color.matrix(), sizeof in.colorM);
    memcpy(in.colorCtl, color.control(), sizeof in.colorCtl);
    in.resetTemporal = g.resetTemporal.exchange(false);
    // The scheduled amount drifts over time; reprocess when it changes even if the screen is idle.
    float colorNow[16];
    memcpy(colorNow, in.colorM, sizeof in.colorM);
    memcpy(colorNow + 12, in.colorCtl, sizeof in.colorCtl);
    bool colorChanged = memcmp(colorNow, lastColor, sizeof lastColor) != 0;
    memcpy(lastColor, colorNow, sizeof lastColor);
    in.force = force || in.resetTemporal || colorChanged;
    in.settleMs = (float)(std::max(params.get("attackMs", 0), params.get("releaseMs", 500)) * 7 + 250);

    // Capture + process each monitor.
    bool retry = now - lastRetry > std::chrono::milliseconds(500);
    if (retry) lastRetry = now;
    for (auto& m : monitors) {
      if (m->lost()) {
        if (retry) m->reacquire();
        continue;
      }
      bool fresh = m->capture();
      m->process(in, fresh);
    }

    // Stats for the meters (5 Hz).
    if (now - lastStats > std::chrono::milliseconds(200)) {
      lastStats = now;
      json::Array arr;
      for (auto& m : monitors) {
        MonitorStats s;
        m->readStats(in, s);
        arr.push_back(json::Object{{"index", m->index}, {"fps", s.fps}, {"frameMs", s.frameMs}, {"inMean", s.inMean},
                                   {"outMean", s.outMean}, {"minGain", s.minGain}, {"brightMean", s.brightMean},
                                   {"lost", m->lost()}});
      }
      std::lock_guard lk(g.mu);
      g.stats = json::Object{{"monitors", arr},
                             {"groupStatus", json::Object{{"color", color.statusColor()},
                                                          {"schedule", color.statusSchedule()}}}};
    }

    // Preview for the web UI (~8 Hz, only while the UI is polling).
    if (now - lastPreview > std::chrono::milliseconds(120)) {
      lastPreview = now;
      int mon, mode;
      bool wanted;
      {
        std::lock_guard lk(g.mu);
        mon = g.previewMon;
        mode = g.previewMode;
        wanted = now - g.previewRequested < std::chrono::seconds(2);
      }
      if (wanted && mon >= 0 && mon < (int)monitors.size()) {
        std::vector<uint8_t> rgba, jpg;
        int w = 0, h = 0;
        if (monitors[mon]->readPreview(in, mode, rgba, w, h) && encodeJpeg(rgba, w, h, 0.8f, jpg)) {
          std::lock_guard lk(g.mu);
          g.previewJpeg.assign(jpg.begin(), jpg.end());
          g.previewJpegMon = mon;
          g.previewJpegMode = mode;
        }
      }
    }

    if (now - lastTopmost > std::chrono::seconds(2)) {
      lastTopmost = now;
      for (auto& m : monitors) m->reassertTopmost();
    }

    // Pace to the display: wait for the primary monitor's vblank.
    if (monitors.empty() || !monitors[0]->waitVBlank()) Sleep(7);
  }

  logf("shutting down");
  http.stop();
  monitors.clear();
  timeEndPeriod(1);
  return 0;
}
