#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "wgc_core.hpp"  // FrameData

class FrameRenderer {
public:
  FrameRenderer(const std::string& title = "Frame Renderer", int maxFps = 60)
      : title_(title),
        hwnd_(nullptr),
        hdcMem_(nullptr),
        hBitmap_(nullptr),
        pBits_(nullptr),
        width_(0),
        height_(0),
        frontIndex_(0),
        backIndex_(1),
        running_(false),
        fpsFont_(nullptr) {
    frames_[0].rgbaData.clear();
    frames_[1].rgbaData.clear();
  }

  ~FrameRenderer() { Stop(); }

  // 非阻塞提交最新帧
  void SubmitFrame(const FrameData& frame) {
    int writeIndex = backIndex_.load(std::memory_order_relaxed);
    frames_[writeIndex] = frame;  // 直接覆盖

    frontIndex_.store(writeIndex, std::memory_order_release);
    backIndex_.store(1 - writeIndex, std::memory_order_relaxed);
  }

  // Run 返回 false -> 退出
  bool Run() {
    if (!hwnd_) CreateRenderWindow(800, 600);  // 初始窗口

    // 处理所有窗口消息
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return false;
      if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) return false;
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    // 渲染最新帧
    int readIndex = frontIndex_.load(std::memory_order_acquire);
    if (!frames_[readIndex].rgbaData.empty()) {
      RenderFrame(frames_[readIndex]);
    }

    return true;
  }

  void Stop() {
    running_ = false;
    if (fpsFont_) {
      DeleteObject(fpsFont_);
      fpsFont_ = nullptr;
    }
    if (hwnd_) PostMessage(hwnd_, WM_CLOSE, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (hBitmap_) DeleteObject(hBitmap_);
    if (hdcMem_) DeleteDC(hdcMem_);
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    hdcMem_ = nullptr;
    hBitmap_ = nullptr;
    pBits_ = nullptr;
  }

private:
  std::string title_;
  HWND hwnd_;
  HDC hdcMem_;
  HBITMAP hBitmap_;
  void* pBits_;
  uint32_t width_, height_;
  HFONT fpsFont_;

  // 添加成员变量用于计算FPS
  std::chrono::steady_clock::time_point lastTime_ = std::chrono::steady_clock::now();
  int frameCount_ = 0;
  float currentFps_ = 0.0f;

  FrameData frames_[2];
  std::atomic<int> frontIndex_;
  std::atomic<int> backIndex_;
  std::atomic<bool> running_;

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  void CreateRenderWindow(uint32_t w, uint32_t h) {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"FrameRendererClass";
    RegisterClass(&wc);

    hwnd_ = CreateWindowEx(0, wc.lpszClassName, std::wstring(title_.begin(), title_.end()).c_str(),
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                           nullptr, nullptr, hInst, nullptr);

    if (!hwnd_) throw std::runtime_error("Failed to create window");

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    width_ = w;
    height_ = h;

    // 创建内存 DC
    HDC hdc = GetDC(hwnd_);
    hdcMem_ = CreateCompatibleDC(hdc);
    ReleaseDC(hwnd_, hdc);

    CreateOrResizeBitmap(width_, height_);
  }

  void CreateOrResizeBitmap(uint32_t w, uint32_t h) {
    if (hBitmap_) DeleteObject(hBitmap_);

    HDC hdc = GetDC(hwnd_);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(w);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap_ = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits_, nullptr, 0);
    if (!hBitmap_) {
      ReleaseDC(hwnd_, hdc);
      throw std::runtime_error("CreateDIBSection failed");
    }
    SelectObject(hdcMem_, hBitmap_);
    ReleaseDC(hwnd_, hdc);
  }

  void ResizeWindow(uint32_t w, uint32_t h) {
    SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    CreateOrResizeBitmap(w, h);
  }

  void RenderFrame(const FrameData& frame) {
    // 窗口或位图大小改变时重新创建
    if (frame.desc.Width != width_ || frame.desc.Height != height_) {
      width_ = frame.desc.Width;
      height_ = frame.desc.Height;
      ResizeWindow(width_, height_);
    }

    // 拷贝帧数据到内存 DC
    std::memcpy(pBits_, frame.rgbaData.data(), frame.rgbaData.size());

    // FPS 计算（1秒更新一次）
    frameCount_++;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime_).count();
    bool fpsChanged = false;
    if (elapsed >= 1000) {
      float newFps = frameCount_ * 1000.0f / elapsed;
      if (static_cast<int>(newFps) != static_cast<int>(currentFps_)) {
        currentFps_ = newFps;
        fpsChanged = true;
      }
      frameCount_ = 0;
      lastTime_ = now;
    }

    // 初始化 FPS 字体（仅一次）
    if (!fpsFont_) {
      int fontHeight = 32;  // 像素高度
      HDC hdc = GetDC(hwnd_);
      int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
      int logicalHeight = -MulDiv(fontHeight, dpiY, 96);
      fpsFont_ = CreateFont(logicalHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Consolas");
      ReleaseDC(hwnd_, hdc);
    }

    HFONT oldFont = (HFONT)SelectObject(hdcMem_, fpsFont_);
    SetBkMode(hdcMem_, TRANSPARENT);
    SetTextColor(hdcMem_, RGB(255, 0, 0));
    std::wstring fpsText = L"FPS:" + std::to_wstring(static_cast<int>(currentFps_));
    RECT fpsRect = {10, 10, 200, 50};
    FillRect(hdcMem_, &fpsRect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    TextOut(hdcMem_, 10, 10, fpsText.c_str(), (int)fpsText.size());
    SelectObject(hdcMem_, oldFont);

    // 一次性 blit 到窗口
    HDC hdc = GetDC(hwnd_);
    BitBlt(hdc, 0, 0, width_, height_, hdcMem_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, hdc);
  }
};
