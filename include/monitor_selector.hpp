#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <iostream>
#include <set>
#include <string>
#include <vector>

class MonitorSelector {
public:
  struct MonitorInfo {
    HMONITOR hmonitor;
    std::string deviceName;
    RECT monitorRect;
    RECT workRect;
    bool isPrimary;
  };

  std::vector<MonitorInfo> EnumerateMonitors() {
    monitors_.clear();
    EnumDisplayMonitors(nullptr, nullptr, &MonitorSelector::EnumProc,
                        reinterpret_cast<LPARAM>(this));
    return monitors_;
  }

  uintptr_t SelectMonitorByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(monitors_.size())) return 0;
    return reinterpret_cast<uintptr_t>(monitors_[index].hmonitor);
  }

  void ShowMonitorList() const {
    for (size_t i = 0; i < monitors_.size(); ++i) {
      const auto& mon = monitors_[i];
      printf(
          "[%zu] HMONITOR: 0x%p | Device: %-32s | Rect: "
          "(%ld,%ld)-(%ld,%ld)%s\n",
          i, mon.hmonitor, mon.deviceName.c_str(), mon.monitorRect.left, mon.monitorRect.top,
          mon.monitorRect.right, mon.monitorRect.bottom, mon.isPrimary ? " [Primary]" : "");
    }
  }

private:
  std::vector<MonitorInfo> monitors_;

  static BOOL CALLBACK EnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* self = reinterpret_cast<MonitorSelector*>(lParam);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMon, &info)) return TRUE;

    MonitorInfo monInfo;
    monInfo.hmonitor = hMon;
    monInfo.monitorRect = info.rcMonitor;
    monInfo.workRect = info.rcWork;
    monInfo.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // Convert wchar_t device name to UTF-8 std::string
    int len = WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
      std::vector<char> utf8(len);
      WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, utf8.data(), len, nullptr, nullptr);
      monInfo.deviceName = std::string(utf8.data());
    } else {
      monInfo.deviceName = "<unknown>";
    }

    self->monitors_.push_back(monInfo);
    return TRUE;
  }
};
