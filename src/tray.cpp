#include "tray.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "log.h"

namespace {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kIdTimer = 1;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"auto-dark";

enum MenuId : UINT {
  kMenuBypass = 1,
  kMenuOpenPanel,
  kMenuStopPanel,
  kMenuStartup,
  kMenuExit,
  kMenuProfileBase = 100,
};

Tray* g_tray = nullptr;
TrayActions* g_actions = nullptr;
UINT g_taskbarCreated = 0;
bool g_lastBypass = false;
HICON g_iconOn = nullptr, g_iconOff = nullptr;

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

// Draws the icon: a ring with a "dimmed" half, amber while guarding, grey while bypassed.
HICON makeIcon(bool active) {
  const int S = 32;
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof bi;
  bi.bV5Width = S;
  bi.bV5Height = -S;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color) return LoadIcon(nullptr, IDI_APPLICATION);
  auto* px = (uint32_t*)bits;
  const double c = (S - 1) / 2.0, rOut = 15.0, rIn = 10.5;
  uint32_t r = active ? 0xE0 : 0x8A, g = active ? 0x90 : 0x8A, b = active ? 0x2F : 0x8A;
  for (int y = 0; y < S; y++) {
    for (int x = 0; x < S; x++) {
      double d = std::hypot(x - c, y - c);
      double ring = std::clamp(rOut + 0.5 - d, 0.0, 1.0);          // outer disc, antialiased
      double inner = std::clamp(rIn + 0.5 - d, 0.0, 1.0);          // inner disc
      double half = x < c ? 1.0 : 0.35;                             // left half bright, right dimmed
      double a = ring * (1 - inner) + inner * half * 0.9;
      uint32_t A = (uint32_t)std::lround(std::clamp(a, 0.0, 1.0) * 255);
      // premultiplied for 32bpp icons
      px[y * S + x] = (A << 24) | ((r * A / 255) << 16) | ((g * A / 255) << 8) | (b * A / 255);
    }
  }
  HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
  ICONINFO ii{TRUE, 0, 0, mask, color};
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

void setIcon(HWND hwnd, DWORD msg, bool bypass) {
  NOTIFYICONDATAW nid{sizeof nid};
  nid.hWnd = hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  nid.uCallbackMessage = kMsgTray;
  nid.hIcon = bypass ? g_iconOff : g_iconOn;
  wcscpy_s(nid.szTip, bypass ? L"auto-dark: bypassed" : L"auto-dark: guarding");
  Shell_NotifyIconW(msg, &nid);
  if (msg == NIM_ADD) {
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }
}

void showMenu(HWND hwnd) {
  TrayState st = g_actions->state();
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
              (L"auto-dark  ·  " + widen(st.activeProfile) + (st.bypass ? L"  (bypassed)" : L"")).c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (st.bypass ? MF_CHECKED : 0), kMenuBypass, L"Bypass\tCtrl+Alt+F12");
  HMENU profiles = CreatePopupMenu();
  for (size_t i = 0; i < st.profiles.size(); i++)
    AppendMenuW(profiles, MF_STRING | (st.profiles[i] == st.activeProfile ? MF_CHECKED : 0),
                kMenuProfileBase + (UINT)i, widen(st.profiles[i]).c_str());
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)profiles, L"Profile");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuOpenPanel, L"Open tuning panel");
  if (st.panelRunning) AppendMenuW(menu, MF_STRING, kMenuStopPanel, L"Stop tuning panel");
  AppendMenuW(menu, MF_STRING | (st.startup ? MF_CHECKED : 0), kMenuStartup, L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
  SetMenuDefaultItem(menu, kMenuOpenPanel, FALSE);

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);  // required so the menu closes when clicking elsewhere
  UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
  PostMessageW(hwnd, WM_NULL, 0, 0);

  if (cmd == kMenuBypass) g_actions->toggleBypass();
  else if (cmd == kMenuOpenPanel) g_actions->openPanel();
  else if (cmd == kMenuStopPanel) g_actions->stopPanel();
  else if (cmd == kMenuStartup) {
    bool on = !Tray::startupEnabled();
    logf("tray: start with Windows %s (%s)", on ? "on" : "off", Tray::setStartup(on) ? "ok" : "failed");
  } else if (cmd == kMenuExit) g_actions->exit();
  else if (cmd >= kMenuProfileBase && cmd < kMenuProfileBase + st.profiles.size())
    g_actions->loadProfile(st.profiles[cmd - kMenuProfileBase]);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted: re-add the icon
    setIcon(hwnd, NIM_ADD, g_lastBypass);
    return 0;
  }
  switch (msg) {
    case kMsgTray:
      switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_LBUTTONUP: showMenu(hwnd); return 0;
        case WM_LBUTTONDBLCLK: g_actions->openPanel(); return 0;
      }
      return 0;
    case Tray::kMsgOpenPanel: g_actions->openPanel(); return 0;
    case WM_TIMER: {  // keep the icon in sync with bypass changes from hotkeys / the panel
      bool b = g_actions->state().bypass;
      if (b != g_lastBypass) {
        g_lastBypass = b;
        setIcon(hwnd, NIM_MODIFY, b);
      }
      return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void Tray::start(TrayActions actions) {
  actions_ = std::move(actions);
  thread_ = std::thread([this] { run(); });
}

void Tray::stop() {
  if (!thread_.joinable()) return;
  PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
  thread_.join();
}

void Tray::run() {
  threadId_ = GetCurrentThreadId();
  g_tray = this;
  g_actions = &actions_;
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  g_iconOn = makeIcon(true);
  g_iconOff = makeIcon(false);

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = TrayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, kWindowClass, L"auto-dark", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                              wc.hInstance, nullptr);
  g_lastBypass = actions_.state().bypass;
  setIcon(hwnd, NIM_ADD, g_lastBypass);
  SetTimer(hwnd, kIdTimer, 300, nullptr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_QUIT) break;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  NOTIFYICONDATAW nid{sizeof nid};
  nid.hWnd = hwnd;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  DestroyWindow(hwnd);
  DestroyIcon(g_iconOn);
  DestroyIcon(g_iconOff);
}

bool Tray::startupEnabled() {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof buf;
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

bool Tray::setStartup(bool on) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
  LSTATUS rc;
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    rc = RegDeleteValueW(key, kRunValue);
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}
