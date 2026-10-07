// Notification-area (tray) icon on its own UI thread, so an open menu never stalls rendering.
#pragma once
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct TrayState {
  bool bypass = false;
  bool panelRunning = false;
  bool startup = false;  // "Start with Windows" (HKCU Run entry)
  std::vector<std::string> profiles;
  std::string activeProfile;
};

struct TrayActions {
  std::function<TrayState()> state;
  std::function<void()> toggleBypass, openPanel, stopPanel, exit;
  std::function<void(const std::string&)> loadProfile;
};

class Tray {
 public:
  ~Tray() { stop(); }
  void start(TrayActions actions);
  void stop();
  // Window that a second instance can message to ask for the panel.
  static constexpr const wchar_t* kWindowClass = L"AutoDarkTray";
  static constexpr unsigned kMsgOpenPanel = 0x8000 + 2;  // WM_APP + 2

  static bool startupEnabled();
  static bool setStartup(bool on);

 private:
  void run();
  std::thread thread_;
  unsigned long threadId_ = 0;
  TrayActions actions_;
};
