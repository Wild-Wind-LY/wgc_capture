#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

class WindowSelector {
public:
  struct WindowInfo {
    uintptr_t hwnd;
    std::string title;
    std::string className;
  };

  void AddTitleBlacklist(const std::string& title) { titleBlacklist_.insert(title); }

  void AddClassBlacklist(const std::string& className) { classBlacklist_.insert(className); }

  std::vector<WindowInfo> EnumerateWindows() {
    windows_.clear();
    EnumWindows(&WindowSelector::EnumProc, reinterpret_cast<LPARAM>(this));
    return windows_;
  }

  uintptr_t SelectWindowByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(windows_.size())) return 0;
    return windows_[index].hwnd;
  }

  bool ActivateWindow(uintptr_t hwnd) const {
    if (!hwnd || !IsWindow(reinterpret_cast<HWND>(hwnd))) return false;

    HWND hWnd = reinterpret_cast<HWND>(hwnd);
    if (IsIconic(hWnd)) {
      ShowWindow(hWnd, SW_RESTORE);
    } else {
      ShowWindow(hWnd, SW_SHOW);
    }
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    return true;
  }

  void ShowWindowList() const {
    for (size_t i = 0; i < windows_.size(); ++i) {
      printf("[%zu] HWND: 0x%08llX | Class: %-20s | Title: %s\n", i,
             static_cast<unsigned long long>(windows_[i].hwnd), windows_[i].className.c_str(),
             windows_[i].title.c_str());
    }
  }

private:
  std::vector<WindowInfo> windows_;
  std::set<std::string> titleBlacklist_;
  std::set<std::string> classBlacklist_;

  static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string result(len - 1, 0);  // exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
  }

  static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam) {
    auto* self = reinterpret_cast<WindowSelector*>(lParam);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (style & WS_CHILD) return TRUE;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
      return TRUE;

    wchar_t wTitle[256]{};
    GetWindowTextW(hwnd, wTitle, 256);
    std::string title = WideToUtf8(wTitle);

    wchar_t realClass[256]{};
    RealGetWindowClassW(hwnd, realClass, 256);
    std::string realClassName = WideToUtf8(realClass);

    wchar_t classNameBuf[256]{};
    GetClassNameW(hwnd, classNameBuf, 256);
    std::string className = WideToUtf8(classNameBuf);

    if (self->titleBlacklist_.count(title)) return TRUE;
    if (self->classBlacklist_.count(className)) return TRUE;
    if (self->classBlacklist_.count(realClassName)) return TRUE;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
      return TRUE;

    self->windows_.emplace_back(WindowInfo{reinterpret_cast<uintptr_t>(hwnd), title, className});

    return TRUE;
  }
};
