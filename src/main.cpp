// auto-dark: always-on spatial luminance limiter ("flashbang guard").
// Backend A: Desktop Duplication capture -> GPU budget pipeline -> click-through dimming overlay.
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <avrt.h>
#include <dwmapi.h>
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
#include "tray.h"

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
  json::Value probe = json::Object{};  // test probe samples + pre-dim events (QPC-based ms)
  bool captureExcluded = true;
  std::atomic<bool> bypass{false};
  std::atomic<bool> resetTemporal{false};
  std::atomic<bool> quit{false};
  std::atomic<int> panelRequest{0};  // 1 = start/open the tuning panel, 2 = stop it (from the tray)
  std::atomic<bool> panelRunning{false};
  // Test hooks for the recovery paths (/api/control).
  std::atomic<bool> simulateDeviceLoss{false}, simulateCaptureLoss{false};
  // preview: requested by the UI, rendered by the render thread
  int previewMon = 0, previewMode = 0;
  std::chrono::steady_clock::time_point previewRequested{};
  std::string previewJpeg;
  int previewJpegMon = -1, previewJpegMode = -1;
};

Shared g;

// ---- "Start protected": windows that just appeared get dimmed before their content is captured.
struct PreDimEvent {
  RECT rect;
  std::chrono::steady_clock::time_point t;
  int64_t qpc;
};
std::vector<PreDimEvent> g_preDims;  // main thread only (WinEvent callbacks run on it)
// Desktop memory: the wallpaper rarely changes, so remember whether the desktop was bright when it
// was last in front, and only pre-dim a desktop reveal if it was (or if we don't know yet).
int g_desktopBright = -1;  // -1 unknown, 0 dark, 1 bright
std::chrono::steady_clock::time_point g_desktopFgSince{};
bool g_desktopFg = false, g_desktopMeasured = false;
bool g_preDimNew = false;
ParamStore* g_paramsForHooks = nullptr;

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd || !g_paramsForHooks) return;
  const ParamStore& p = *g_paramsForHooks;
  // Revealing the desktop (Win+D, minimize all) brings the wallpaper forward without showing a new
  // window: treat the desktop becoming foreground like a newly shown window.
  wchar_t cls[32] = {};
  GetClassNameW(hwnd, cls, 32);
  bool desktop = event == EVENT_SYSTEM_FOREGROUND && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"));
  if (event == EVENT_SYSTEM_FOREGROUND) {
    g_desktopFg = desktop;
    g_desktopMeasured = false;
    if (desktop) g_desktopFgSince = std::chrono::steady_clock::now();
  }
  if (desktop) {
    if (p.get("preDimShow", 1) < 0.5 || g_desktopBright == 0) return;
    RECT vr{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    vr.right = vr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vr.bottom = vr.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_preDims.push_back({vr, std::chrono::steady_clock::now(), q.QuadPart});
    if (g_preDims.size() > 8) g_preDims.erase(g_preDims.begin());
    g_preDimNew = true;
    return;
  }
  bool want = (event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_MINIMIZEEND) ? p.get("preDimShow", 1) >= 0.5
              : event == EVENT_SYSTEM_FOREGROUND                                ? p.get("preDimForeground", 0) >= 0.5
              : event == EVENT_OBJECT_NAMECHANGE                                ? p.get("preDimTitle", 0) >= 0.5
                                                                                : false;
  if (!want || GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd)) return;
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return;
  RECT r{};
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) &&
      !GetWindowRect(hwnd, &r))
    return;
  if (r.right <= r.left || r.bottom <= r.top) return;
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  g_preDims.push_back({r, std::chrono::steady_clock::now(), q.QuadPart});
  if (g_preDims.size() > 8) g_preDims.erase(g_preDims.begin());
  g_preDimNew = true;
}

// ---- Fullscreen apps: a game (or video) covering a whole monitor gets that monitor's overlay hidden.
// Browsers stay guarded in "games" mode since fullscreen web video is exactly what we protect against.
bool isBrowserProcess(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return false;
  wchar_t path[MAX_PATH];
  DWORD n = MAX_PATH;
  bool ok = QueryFullProcessImageNameW(h, 0, path, &n);
  CloseHandle(h);
  if (!ok) return false;
  const wchar_t* exe = wcsrchr(path, L'\\');
  exe = exe ? exe + 1 : path;
  static const wchar_t* kBrowsers[] = {L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
                                       L"opera.exe",  L"vivaldi.exe", L"arc.exe",    L"librewolf.exe"};
  for (auto b : kBrowsers)
    if (!_wcsicmp(exe, b)) return true;
  return false;
}

// The desktop rect of the monitor the foreground window fills, or false. mode: 1 = games (non-browser), 2 = any.
bool fullscreenForeground(int mode, RECT& out) {
  HWND fg = GetForegroundWindow();
  if (!fg || mode <= 0 || !IsWindowVisible(fg) || IsIconic(fg)) return false;
  wchar_t cls[32] = {};
  GetClassNameW(fg, cls, 32);
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
  // Maximized normal windows also cover the monitor (auto-hide taskbar); fullscreen ones have no caption.
  if ((GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false;
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return false;
  RECT r;
  if (!GetWindowRect(fg, &r)) return false;
  MONITORINFO mi{sizeof mi};
  if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONULL), &mi)) return false;
  const RECT& m = mi.rcMonitor;
  if (r.left > m.left || r.top > m.top || r.right < m.right || r.bottom < m.bottom) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == GetCurrentProcessId() || (mode == 1 && isBrowserProcess(pid))) return false;
  out = m;
  return true;
}

// Worst-case gain for a rect of area fraction `a` on a monitor, assuming it is entirely white:
// its own neighbourhood budget, and the whole-screen budget spread over it.
float preDimGainFor(const ParamStore& p, const std::vector<std::pair<double, double>>& curve, double a) {
  a = std::clamp(a, 1e-7, 1.0);
  double gain = std::min({1.0, ParamStore::evalCurve(curve, std::log10(a)), ParamStore::evalCurve(curve, 0.0) / a});
  gain = 1 + (gain - 1) * p.get("strength", 1);
  return (float)std::max(gain, p.get("minGain", 0.05));
}

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

  if (req.method == "GET" && p == "/api/probe") {
    std::lock_guard lk(g.mu);
    return jsonResponse(g.probe.clone());
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
    if (body.has("simulateDeviceLoss")) g.simulateDeviceLoss = true;
    if (body.has("simulateCaptureLoss")) g.simulateCaptureLoss = true;
    if (body.has("quit") && body["quit"].num() != 0) g.quit = true;
    return okOrError(true, "");
  }

  return jsonResponse(json::Object{{"error", "not found"}}, 404);
}

struct Args {
  int port = 7777;
  bool bypass = false;
  bool open = false;
  bool panel = false;    // start the tuning panel's web server at launch
  bool console = false;  // show a console window with the log
  bool exclude = true;
  bool debugLayer = false;
  bool vblankPacing = false;  // old loop timing, for A/B latency comparison
  int probeX = -1, probeY = -1;  // test probe pixel (desktop coordinates)
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
    else if (s == L"--open") a.open = a.panel = true;
    else if (s == L"--panel") a.panel = true;
    else if (s == L"--console") a.console = true;
    else if (s == L"--no-exclude") a.exclude = false;
    else if (s == L"--debug-layer") a.debugLayer = true;
    else if (s == L"--vblank") a.vblankPacing = true;
    else if (s == L"--probe" && i + 2 < argc) {
      a.probeX = _wtoi(argv[++i]);
      a.probeY = _wtoi(argv[++i]);
    }
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
  // Ask the GPU scheduler to favour our (tiny) workload so the overlay is ready before DWM composes.
  HRESULT pr = dxgiDev->SetGPUThreadPriority(7);
  logf("gpu: thread priority 7 %s", SUCCEEDED(pr) ? "set" : "not allowed (needs admin)");
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
  Args args = parseArgs();
  // One engine at a time: a second launch asks the running one to open its tuning panel.
  HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\auto-dark-engine");  // held for the process lifetime
  (void)instance;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(Tray::kWindowClass, nullptr)) PostMessageW(other, Tray::kMsgOpenPanel, 0, 0);
    return 0;
  }
  if (args.console && AllocConsole()) {
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  timeBeginPeriod(1);

  std::string root = findRoot();
  fs::create_directories(fs::u8path(root + "/logs"));
  logToFile((root + "/logs/auto-dark.log").c_str());
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

  // Capture threads signal this the moment Windows presents a new desktop image.
  HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto monitors = buildMonitors(gpu, args.exclude);
  for (auto& m : monitors) m->startCapture(wake);
  publishMonitors(monitors);

  // The render loop is the rest of the latency chain: real-time-ish priority.
  DWORD mmTask = 0;
  HANDLE mmHandle = AvSetMmThreadCharacteristicsW(L"Games", &mmTask);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  logf("render thread: MMCSS %s", mmHandle ? "on" : "unavailable");
  DWM_TIMING_INFO dti{sizeof dti};
  double refreshMs = 1000.0 / 144;
  if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &dti)) && dti.rateRefresh.uiNumerator)
    refreshMs = 1000.0 * dti.rateRefresh.uiDenominator / dti.rateRefresh.uiNumerator;
  logf("display: composition refresh %.2f ms", refreshMs);
  if (!args.exclude) logf("WARNING: --no-exclude: the overlay is visible to capture (debug only, feedback expected)");

  RegisterHotKey(nullptr, kHotBypass, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12);
  RegisterHotKey(nullptr, kHotProfile, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11);
  RegisterHotKey(nullptr, kHotDebug, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
  logf("hotkeys: Ctrl+Alt+F12 bypass, Ctrl+Alt+F11 next profile, Ctrl+Alt+F10 debug view");

  // The tuning panel's web server only runs on request (tray menu, --panel, --open).
  HttpServer http;
  std::wstring panelUrl = L"http://127.0.0.1:" + std::to_wstring(args.port) + L"/";
  auto startPanel = [&] {
    if (g.panelRunning) return true;
    bool ok = http.start(args.port, [&](const HttpRequest& req) {
      if (req.path.rfind("/api/", 0) == 0) return handleApi(root, params, req);
      if (req.method != "GET") return jsonResponse(json::Object{{"error", "not found"}}, 404);
      return serveStatic(root, req.path);
    });
    g.panelRunning = ok;
    return ok;
  };
  auto stopPanel = [&] {
    if (!g.panelRunning) return;
    http.stop();
    g.panelRunning = false;
    logf("http: tuning panel stopped");
  };
  if (args.panel) startPanel();

  Tray tray;
  tray.start(TrayActions{
      [&] {
        return TrayState{g.bypass.load(), g.panelRunning.load(), Tray::startupEnabled(), params.listProfiles(),
                         params.activeProfile()};
      },
      [&] {
        g.bypass = !g.bypass;
        logf("bypass %s (tray)", g.bypass ? "ON" : "off");
      },
      [&] {
        g.panelRequest = 1;
        SetEvent(wake);
      },
      [&] {
        g.panelRequest = 2;
        SetEvent(wake);
      },
      [&] {
        g.quit = true;
        SetEvent(wake);
      },
      [&](const std::string& name) {
        std::string err;
        if (params.loadProfile(name, err)) g.resetTemporal = true;
        else logf("tray: %s", err.c_str());
      },
  });
  g_paramsForHooks = &params;
  HWINEVENTHOOK hooks[] = {
      SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWinEvent, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
      SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
      SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
      SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, OnWinEvent, 0, 0,
                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
  };
  if (args.probeX >= 0)
    for (auto& m : monitors)
      if (args.probeX >= m->desktop.left && args.probeX < m->desktop.right && args.probeY >= m->desktop.top &&
          args.probeY < m->desktop.bottom)
        m->enableProbe(args.probeX, args.probeY);

  if (args.open && g.panelRunning) ShellExecuteW(nullptr, L"open", panelUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

  ColorPlan color;
  float lastColor[16] = {};

  ComPtr<ID3D11Buffer> paramsCB;
  uint32_t paramsCBSize = 0;
  uint64_t paramsVersion = 0;
  const ShaderSet* paramsShaderSet = nullptr;

  using clock = std::chrono::steady_clock;
  auto start = clock::now(), last = start, lastPoll = start, lastStats = start, lastPreview = start,
       lastTopmost = start, lastHealth = start, lastFullscreen = start;
  // A monitor's capture reset waits until its capture errors have been quiet this long, and runs at
  // most once per gap, in case the errors keep coming back.
  constexpr uint64_t kCaptureQuietMs = 1500, kCaptureResetGapMs = 5000;

  while (!g.quit) {
    // Tray requests for the tuning panel.
    if (int req = g.panelRequest.exchange(0)) {
      if (req == 1 && startPanel()) ShellExecuteW(nullptr, L"open", panelUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      if (req == 2) stopPanel();
    }

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

    // Render device health (cheap; every half second): a removed device would freeze the overlay.
    if (now - lastHealth > std::chrono::milliseconds(500)) {
      lastHealth = now;
      HRESULT dr = gpu.dev->GetDeviceRemovedReason();
      if (FAILED(dr)) {
        logf("gpu: render device removed (0x%08lx)", (unsigned long)dr);
        g_deviceLost = true;
      }
    }
    bool deviceLost = g_deviceLost.exchange(false);
    if (g.simulateDeviceLoss.exchange(false)) deviceLost = true;
    if (deviceLost) {
      logf("gpu: recreating devices, shaders and monitors");
      monitors.clear();
      paramsCB.Reset();
      paramsShaderSet = nullptr;
      gpu = Gpu{};
      while (!createGpu(args.debugLayer, gpu) && !g.quit) {
        logf("gpu: device creation failed, retrying");
        Sleep(1000);
      }
      std::string err;
      if (!shaders.compile(gpu.dev.Get(), err)) logf("shaders: %s", err.c_str());
      g_displayChanged = true;  // rebuild monitors on the new device below
    }

    // A monitor whose capture errors have gone quiet gets a fresh capture device (see
    // Monitor::resetCapture). Its overlay stays up throughout, and the other monitors are untouched.
    for (auto& m : monitors) {
      uint64_t errorTick = m->captureErrorTick();
      if (!errorTick) continue;
      uint64_t tick = GetTickCount64();  // read after errorTick, so it is never older
      if (tick - errorTick <= kCaptureQuietMs || tick - m->lastCaptureReset <= kCaptureResetGapMs) continue;
      m->lastCaptureReset = tick;
      logf("monitor %d: capture errors stopped; resetting its capture device", m->index);
      std::string err;
      if (!m->resetCapture(err)) {
        logf("monitor %d: capture reset failed (%s); rebuilding monitors", m->index, err.c_str());
        g_displayChanged = true;
      }
    }

    bool displayChanged = g_displayChanged.exchange(false);
    if (g.simulateCaptureLoss.exchange(false)) displayChanged = true;
    if (displayChanged) {
      logf("display/capture changed; rebuilding monitors");
      monitors.clear();
      Sleep(500);
      monitors = buildMonitors(gpu, args.exclude);
      for (auto& m : monitors) m->startCapture(wake);
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
    in.minIntervalMs = (float)(refreshMs * 0.8);
    in.settleMs = (float)(std::max(params.get("attackMs", 0), params.get("releaseMs", 500)) * 7 + 250);

    // Start protected: active pre-dims (hold, then fade out once real measurements have taken over).
    {
      double hold = params.get("preDimHoldMs", 120), fade = std::max(1.0, params.get("preDimFadeMs", 80));
      double minArea = params.get("preDimMinArea", 0.05);
      g_preDims.erase(std::remove_if(g_preDims.begin(), g_preDims.end(),
                                     [&](const PreDimEvent& e) {
                                       return std::chrono::duration<double, std::milli>(now - e.t).count() > hold + fade;
                                     }),
                      g_preDims.end());
      for (auto& m : monitors) {
        RECT rects[4];
        float gains[4];
        int n = 0;
        double monArea = (double)m->width * m->height;
        for (auto it = g_preDims.rbegin(); it != g_preDims.rend() && n < 4; ++it) {
          RECT r;
          if (!IntersectRect(&r, &it->rect, &m->desktop)) continue;
          double a = (double)(r.right - r.left) * (r.bottom - r.top) / monArea;
          if (a < minArea) continue;
          double age = std::chrono::duration<double, std::milli>(now - it->t).count();
          double f = age < hold ? 1 : 1 - (age - hold) / fade;
          float g0 = preDimGainFor(params, in.curve, a);
          rects[n] = r;
          gains[n] = (float)(1 + (g0 - 1) * std::clamp(f, 0.0, 1.0));
          n++;
        }
        m->setPreDim(rects, gains, in.bypass ? 0 : n);
      }
      if (!g_preDims.empty()) in.force = true;  // render immediately and keep fading
      g_preDimNew = false;
    }

    // Process each monitor with its newest captured image (capture runs on its own threads).
    bool animating = false;
    for (auto& m : monitors) animating |= m->process(in);

    // Stats (5 Hz): the panel's meters, and the desktop-brightness memory.
    if (now - lastStats > std::chrono::milliseconds(200)) {
      lastStats = now;
      json::Array arr;
      double strongestDim = 1;
      for (auto& m : monitors) {
        MonitorStats s;
        m->readStats(in, s);
        if (!m->lost()) strongestDim = std::min(strongestDim, s.minGain);
        arr.push_back(json::Object{{"index", m->index}, {"fps", s.fps}, {"frameMs", s.frameMs},
                                   {"latMs", s.latMs}, {"latMaxMs", s.latMaxMs}, {"acqMs", s.acqMs},
                                   {"refreshMs", refreshMs}, {"inMean", s.inMean},
                                   {"outMean", s.outMean}, {"minGain", s.minGain}, {"brightMean", s.brightMean},
                                   {"lost", m->lost()}});
      }
      // Desktop has been in front for a moment: remember whether the guard had to dim it.
      if (g_desktopFg && !g_desktopMeasured &&
          std::chrono::duration<double, std::milli>(now - g_desktopFgSince).count() > 600) {
        g_desktopMeasured = true;
        int bright = strongestDim < 0.95 ? 1 : 0;
        if (bright != g_desktopBright) logf("desktop memory: wallpaper %s", bright ? "bright" : "dark");
        g_desktopBright = bright;
      }
      if (args.probeX >= 0) {
        static const double qpcPerMs = [] {
          LARGE_INTEGER f;
          QueryPerformanceFrequency(&f);
          return f.QuadPart / 1000.0;
        }();
        json::Array samples, events;
        for (auto& m : monitors)
          for (auto& s : m->probeSamples())
            samples.push_back(json::Array{s.qpc / qpcPerMs, (double)s.r, (double)s.g, (double)s.b});
        for (auto& e : g_preDims) events.push_back(e.qpc / qpcPerMs);
        std::lock_guard lk(g.mu);
        (*g.probe.o)["samples"] = json::Value(std::move(samples));
        auto& ev = (*g.probe.o)["preDimEvents"];
        if (!ev.isArr()) ev = json::Array{};
        for (auto& e : events) {
          bool seen = false;
          for (auto& x : *ev.a) seen |= x.num() == e.num();
          if (!seen) ev.a->push_back(e);
        }
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
        int w = 0, h = 0, gotMode = mode;
        if (monitors[mon]->readPreview(in, mode, rgba, w, h, gotMode) && encodeJpeg(rgba, w, h, 0.8f, jpg)) {
          std::lock_guard lk(g.mu);
          g.previewJpeg.assign(jpg.begin(), jpg.end());
          g.previewJpegMon = mon;
          g.previewJpegMode = gotMode;
        }
      }
    }

    // Fullscreen app in front (4 Hz): hide that monitor's overlay until it leaves.
    if (now - lastFullscreen > std::chrono::milliseconds(250)) {
      lastFullscreen = now;
      RECT fs{};
      bool any = fullscreenForeground((int)params.get("fullscreenPause", 1), fs);
      for (auto& m : monitors) {
        bool want = any && EqualRect(&fs, &m->desktop);
        if (want == m->suspended()) continue;
        m->setSuspended(want);
        logf("monitor %d: %s (fullscreen app)", m->index, want ? "paused" : "resumed");
        if (!want) g.resetTemporal = true;  // the state is from before the app; start fresh
      }
    }

    if (now - lastTopmost > std::chrono::seconds(2)) {
      lastTopmost = now;
      for (auto& m : monitors) m->reassertTopmost();
    }

    // Pace to the display: wait for the primary monitor's vblank.
    // Sleep until a capture thread has a new frame (or a message arrives). While the temporal
    // state is still animating, also wake once per refresh; otherwise idle cheaply.
    if (args.vblankPacing) {
      if (monitors.empty() || !monitors[0]->waitVBlank()) Sleep(7);
    } else {
      DWORD timeout = animating ? (DWORD)std::max(1.0, refreshMs) : 25;
      MsgWaitForMultipleObjectsEx(1, &wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
  }

  logf("shutting down");
  tray.stop();
  stopPanel();
  for (auto h : hooks)
    if (h) UnhookWinEvent(h);
  monitors.clear();
  CloseHandle(wake);
  if (mmHandle) AvRevertMmThreadCharacteristics(mmHandle);
  timeEndPeriod(1);
  return 0;
}
