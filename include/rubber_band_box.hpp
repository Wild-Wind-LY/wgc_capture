#pragma once

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class RubberBandBox {
public:
  using RectCallback = std::function<void(RECT)>;

  RubberBandBox(HINSTANCE hInstance, RectCallback cb) : hInstance_(hInstance), callback_(cb) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 创建 D2D 工厂
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory_);

    // 创建 WIC 工厂
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&pwicFactory_));

    // 创建 DirectWrite 工厂
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&pDWriteFactory_));

    // 创建一个文本格式
    if (pDWriteFactory_) {
      pDWriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
                                        L"zh-CN", &pTextFormat_);

      // 左上对齐
      pTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      pTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
  }

  ~RubberBandBox() {
    FreeResources();
    if (pFactory_) pFactory_->Release();
    if (pwicFactory_) pwicFactory_->Release();
    CoUninitialize();
  }

  static std::optional<RECT> SelectRegion(HINSTANCE hInstance) {
    RECT result{};
    bool got = false;

    RubberBandBox box(hInstance, [&](RECT rect) {
      result = rect;
      got = true;
    });

    if (!box.Create()) return std::nullopt;

    box.RunMessageLoop();

    if (got) return result;
    return std::nullopt;
  }

private:
  enum class DragMode {
    None,
    Move,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
  };

  // ---- 成员 ----
  HINSTANCE hInstance_;
  HWND hwnd_ = nullptr;
  bool isSelecting_ = false;
  bool isMoving_ = false;
  bool hasSelection_ = false;
  DragMode dragMode_ = DragMode::None;
  POINT startPoint_{};
  POINT currentPoint_{};
  POINT dragOffset_{};
  POINT lastMousePoint_{};
  RECT selectedRect_{};
  RECT dragStartRect_{};
  RectCallback callback_;

  // D2D / WIC
  ID2D1Factory* pFactory_ = nullptr;
  IWICImagingFactory* pwicFactory_ = nullptr;
  IWICBitmap* pwicBitmap_ = nullptr;
  ID2D1RenderTarget* pRenderTarget_ = nullptr;

  IDWriteFactory* pDWriteFactory_ = nullptr;
  IDWriteTextFormat* pTextFormat_ = nullptr;

  // DIB for UpdateLayeredWindow
  HDC memDC_ = nullptr;
  HBITMAP hDib_ = nullptr;
  void* dibBits_ = nullptr;
  int originX_ = 0;
  int originY_ = 0;
  int screenW_ = 0;
  int screenH_ = 0;

  // ---- 窗口回调/调度 ----
  static LRESULT CALLBACK WindowProcStatic(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    RubberBandBox* self = nullptr;
    if (uMsg == WM_NCCREATE) {
      CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
      self = reinterpret_cast<RubberBandBox*>(cs->lpCreateParams);
      SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
      self->hwnd_ = hwnd;
    } else {
      self = reinterpret_cast<RubberBandBox*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    return self ? self->WindowProc(uMsg, wParam, lParam)
                : DefWindowProc(hwnd, uMsg, wParam, lParam);
  }

  LRESULT WindowProc(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
      case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetFocus(hwnd_);
        lastMousePoint_ = pt;
        dragMode_ = HitTestSelection(pt);
        if (hasSelection_ && dragMode_ != DragMode::None) {
          isMoving_ = true;
          dragStartRect_ = selectedRect_;
          dragOffset_.x = pt.x - selectedRect_.left;
          dragOffset_.y = pt.y - selectedRect_.top;
        } else {
          isSelecting_ = true;
          dragMode_ = DragMode::None;
          hasSelection_ = false;
          startPoint_ = pt;
          currentPoint_ = pt;
        }
        SetCapture(hwnd_);
        RenderAndUpdateWindow();
        break;
      }

      case WM_MOUSEMOVE:
        lastMousePoint_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (isSelecting_) {
          POINT pt = lastMousePoint_;
          if (pt.x != currentPoint_.x || pt.y != currentPoint_.y) {
            currentPoint_ = pt;
            RenderAndUpdateWindow();
          }
        } else if (isMoving_ && hasSelection_) {
          POINT pt = lastMousePoint_;
          selectedRect_ = ComputeDraggedRect(pt);
          currentPoint_ = {selectedRect_.right, selectedRect_.bottom};
          startPoint_ = {selectedRect_.left, selectedRect_.top};
          RenderAndUpdateWindow();
        } else if (hasSelection_) {
          SetCursor(GetCursorForMode(HitTestSelection(lastMousePoint_)));
        }
        break;

      case WM_LBUTTONUP:
        if (isSelecting_) {
          isSelecting_ = false;
          ReleaseCapture();
          RECT r = MakeRect(startPoint_, currentPoint_);
          if (r.right > r.left && r.bottom > r.top) {
            selectedRect_ = r;
            hasSelection_ = true;
            startPoint_ = {r.left, r.top};
            currentPoint_ = {r.right, r.bottom};
          }
          RenderAndUpdateWindow();
        } else if (isMoving_) {
          isMoving_ = false;
          dragMode_ = DragMode::None;
          ReleaseCapture();
          RenderAndUpdateWindow();
        }
        break;

      case WM_LBUTTONDBLCLK:
        if (hasSelection_ && callback_) {
          callback_(ToScreenRect(selectedRect_));
          DestroyWindow(hwnd_);
        }
        break;

      case WM_RBUTTONDOWN:
      case WM_KEYDOWN:
        if (uMsg == WM_RBUTTONDOWN || wParam == VK_ESCAPE) {
          isSelecting_ = false;
          isMoving_ = false;
          dragMode_ = DragMode::None;
          hasSelection_ = false;
          ReleaseCapture();
          DestroyWindow(hwnd_);
          break;
        }
        if (wParam == VK_RETURN && hasSelection_ && callback_) {
          callback_(ToScreenRect(selectedRect_));
          DestroyWindow(hwnd_);
          break;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);

      case WM_CAPTURECHANGED:
        if (isSelecting_ || isMoving_) {
          isSelecting_ = false;
          isMoving_ = false;
          dragMode_ = DragMode::None;
        }
        break;

      case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
          DragMode mode = isMoving_ ? dragMode_ : HitTestSelection(lastMousePoint_);
          SetCursor(GetCursorForMode(mode));
          return TRUE;
        }
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);

      case WM_DESTROY:
        PostQuitMessage(0);
        break;

      default:
        return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
    return 0;
  }

  bool Create() {
    const wchar_t CLASS_NAME[] = L"LayeredRubberBandD2D_WIC";
    WNDCLASS wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProcStatic;
    wc.hInstance = hInstance_;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClass(&wc);

    originX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    originY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screenW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screenH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hwnd_ = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CLASS_NAME, L"",
                           WS_POPUP, originX_, originY_, screenW_, screenH_, nullptr, nullptr,
                           hInstance_, this);

    if (!hwnd_) return false;

    // 准备绘制目标与 DIB（一次性创建并复用）
    if (!InitWicAndD2DResources()) return false;
    if (!InitDibSection()) return false;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    SetActiveWindow(hwnd_);
    SetFocus(hwnd_);
    RenderAndUpdateWindow();
    return true;
  }

  void RunMessageLoop() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  // ---- helper ----
  RECT MakeRect(const POINT& a, const POINT& b) const {
    RECT r;
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    // clamp
    r.left = std::clamp(r.left, 0L, static_cast<LONG>(screenW_));
    r.top = std::clamp(r.top, 0L, static_cast<LONG>(screenH_));
    r.right = std::clamp(r.right, 0L, static_cast<LONG>(screenW_));
    r.bottom = std::clamp(r.bottom, 0L, static_cast<LONG>(screenH_));
    return r;
  }

  D2D1_RECT_F ClampInfoRect(D2D1_RECT_F rect) const {
    const float maxLeft = std::max(0.0f, static_cast<float>(screenW_) - (rect.right - rect.left));
    const float maxTop = std::max(0.0f, static_cast<float>(screenH_) - (rect.bottom - rect.top));
    const float clampedLeft = std::clamp(rect.left, 8.0f, std::max(8.0f, maxLeft - 8.0f));
    const float clampedTop = std::clamp(rect.top, 8.0f, std::max(8.0f, maxTop - 8.0f));
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    return D2D1::RectF(clampedLeft, clampedTop, clampedLeft + width, clampedTop + height);
  }

  RECT GetActiveSelection() const {
    if (hasSelection_) return selectedRect_;
    return MakeRect(startPoint_, currentPoint_);
  }

  RECT ToScreenRect(const RECT& rect) const {
    RECT result = rect;
    result.left += originX_;
    result.right += originX_;
    result.top += originY_;
    result.bottom += originY_;
    return result;
  }

  DragMode HitTestSelection(const POINT& pt) const {
    if (!hasSelection_) return DragMode::None;

    constexpr int kEdgeGrip = 6;
    constexpr int kCornerGrip = 12;

    RECT outer = selectedRect_;
    RECT inner = selectedRect_;
    InflateRect(&outer, kEdgeGrip, kEdgeGrip);
    InflateRect(&inner, -kEdgeGrip, -kEdgeGrip);

    if (!PtInRect(&outer, pt) && !PtInRect(&selectedRect_, pt)) return DragMode::None;

    const bool nearLeft = std::abs(pt.x - selectedRect_.left) <= kCornerGrip;
    const bool nearRight = std::abs(pt.x - selectedRect_.right) <= kCornerGrip;
    const bool nearTop = std::abs(pt.y - selectedRect_.top) <= kCornerGrip;
    const bool nearBottom = std::abs(pt.y - selectedRect_.bottom) <= kCornerGrip;

    if (nearLeft && nearTop) return DragMode::TopLeft;
    if (nearRight && nearTop) return DragMode::TopRight;
    if (nearLeft && nearBottom) return DragMode::BottomLeft;
    if (nearRight && nearBottom) return DragMode::BottomRight;
    if (std::abs(pt.x - selectedRect_.left) <= kEdgeGrip) return DragMode::Left;
    if (std::abs(pt.x - selectedRect_.right) <= kEdgeGrip) return DragMode::Right;
    if (std::abs(pt.y - selectedRect_.top) <= kEdgeGrip) return DragMode::Top;
    if (std::abs(pt.y - selectedRect_.bottom) <= kEdgeGrip) return DragMode::Bottom;
    if (PtInRect(&selectedRect_, pt) || PtInRect(&inner, pt)) return DragMode::Move;
    return DragMode::None;
  }

  HCURSOR GetCursorForMode(DragMode mode) const {
    switch (mode) {
      case DragMode::Left:
      case DragMode::Right:
        return LoadCursor(nullptr, IDC_SIZEWE);
      case DragMode::Top:
      case DragMode::Bottom:
        return LoadCursor(nullptr, IDC_SIZENS);
      case DragMode::TopLeft:
      case DragMode::BottomRight:
        return LoadCursor(nullptr, IDC_SIZENWSE);
      case DragMode::TopRight:
      case DragMode::BottomLeft:
        return LoadCursor(nullptr, IDC_SIZENESW);
      case DragMode::Move:
        return LoadCursor(nullptr, IDC_SIZEALL);
      case DragMode::None:
      default:
        return LoadCursor(nullptr, IDC_CROSS);
    }
  }

  RECT NormalizeRect(RECT rect) const {
    if (rect.left > rect.right) std::swap(rect.left, rect.right);
    if (rect.top > rect.bottom) std::swap(rect.top, rect.bottom);
    rect.left = std::clamp(rect.left, 0L, static_cast<LONG>(screenW_));
    rect.top = std::clamp(rect.top, 0L, static_cast<LONG>(screenH_));
    rect.right = std::clamp(rect.right, 0L, static_cast<LONG>(screenW_));
    rect.bottom = std::clamp(rect.bottom, 0L, static_cast<LONG>(screenH_));
    return rect;
  }

  RECT ComputeDraggedRect(const POINT& pt) const {
    constexpr int kMinSize = 8;

    RECT rect = dragStartRect_;
    if (dragMode_ == DragMode::Move) {
      const int width = dragStartRect_.right - dragStartRect_.left;
      const int height = dragStartRect_.bottom - dragStartRect_.top;
      const int maxLeft = std::max(0, screenW_ - width);
      const int maxTop = std::max(0, screenH_ - height);
      const int newLeft = std::clamp((int)(pt.x - dragOffset_.x), 0, maxLeft);
      const int newTop = std::clamp((int)(pt.y - dragOffset_.y), 0, maxTop);
      rect.left = newLeft;
      rect.top = newTop;
      rect.right = newLeft + width;
      rect.bottom = newTop + height;
      return rect;
    }

    switch (dragMode_) {
      case DragMode::Left:
      case DragMode::TopLeft:
      case DragMode::BottomLeft:
        rect.left = pt.x;
        break;
      default:
        break;
    }
    switch (dragMode_) {
      case DragMode::Right:
      case DragMode::TopRight:
      case DragMode::BottomRight:
        rect.right = pt.x;
        break;
      default:
        break;
    }
    switch (dragMode_) {
      case DragMode::Top:
      case DragMode::TopLeft:
      case DragMode::TopRight:
        rect.top = pt.y;
        break;
      default:
        break;
    }
    switch (dragMode_) {
      case DragMode::Bottom:
      case DragMode::BottomLeft:
      case DragMode::BottomRight:
        rect.bottom = pt.y;
        break;
      default:
        break;
    }

    rect = NormalizeRect(rect);

    if (rect.right - rect.left < kMinSize) {
      if (dragMode_ == DragMode::Left || dragMode_ == DragMode::TopLeft
          || dragMode_ == DragMode::BottomLeft) {
        rect.left = std::max(0L, rect.right - kMinSize);
      } else {
        rect.right = std::min(static_cast<LONG>(screenW_), rect.left + kMinSize);
      }
    }
    if (rect.bottom - rect.top < kMinSize) {
      if (dragMode_ == DragMode::Top || dragMode_ == DragMode::TopLeft
          || dragMode_ == DragMode::TopRight) {
        rect.top = std::max(0L, rect.bottom - kMinSize);
      } else {
        rect.bottom = std::min(static_cast<LONG>(screenH_), rect.top + kMinSize);
      }
    }

    return NormalizeRect(rect);
  }

  D2D1_RECT_F GetInfoRect(const RECT& sel, bool hasRect) const {
    if (!hasRect) return ClampInfoRect(D2D1::RectF(24.0f, 24.0f, 360.0f, 108.0f));

    const float panelW = 380.0f;
    const float panelH = 108.0f;
    const float gap = 12.0f;

    const float left = static_cast<float>(sel.left);
    const float top = static_cast<float>(sel.top);
    const float right = static_cast<float>(sel.right);
    const float bottom = static_cast<float>(sel.bottom);

    if (right + panelW + gap <= static_cast<float>(screenW_) - 8.0f) {
      return ClampInfoRect(D2D1::RectF(right + gap, top, right + panelW + gap, top + panelH));
    }
    if (bottom + panelH + gap <= static_cast<float>(screenH_) - 8.0f) {
      return ClampInfoRect(D2D1::RectF(left, bottom + gap, left + panelW, bottom + panelH + gap));
    }
    if (left >= panelW + gap + 8.0f) {
      return ClampInfoRect(D2D1::RectF(left - panelW - gap, top, left - gap, top + panelH));
    }
    return ClampInfoRect(D2D1::RectF(left, top - panelH - gap, left + panelW, top - gap));
  }

  void FreeResources() {
    if (pRenderTarget_) {
      pRenderTarget_->Release();
      pRenderTarget_ = nullptr;
    }
    if (pwicBitmap_) {
      pwicBitmap_->Release();
      pwicBitmap_ = nullptr;
    }
    if (memDC_) {
      DeleteDC(memDC_);
      memDC_ = nullptr;
    }
    if (hDib_) {
      DeleteObject(hDib_);
      hDib_ = nullptr;
    }

    if (pTextFormat_) {
      pTextFormat_->Release();
      pTextFormat_ = nullptr;
    }
    if (pDWriteFactory_) {
      pDWriteFactory_->Release();
      pDWriteFactory_ = nullptr;
    }
  }

  bool InitWicAndD2DResources() {
    if (!pwicFactory_ || !pFactory_) return false;

    // 创建 WIC Bitmap (32bpp PBGRA) 作为 D2D 的后端
    HRESULT hr = pwicFactory_->CreateBitmap(screenW_, screenH_, GUID_WICPixelFormat32bppPBGRA,
                                            WICBitmapCacheOnLoad, &pwicBitmap_);
    if (FAILED(hr) || !pwicBitmap_) return false;

    // 使用 CreateWicBitmapRenderTarget 创建 D2D RenderTarget
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = pFactory_->CreateWicBitmapRenderTarget(pwicBitmap_, &props, &pRenderTarget_);
    if (FAILED(hr) || !pRenderTarget_) return false;

    // 设置一些默认参数
    pRenderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
  }

  bool InitDibSection() {
    // 创建兼容 DIBSection (32bpp) 并保存 bits 指针以便 memcpy
    HDC screenDC = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = screenW_;
    bmi.bmiHeader.biHeight = -screenH_;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hDib_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
    if (!hDib_) {
      ReleaseDC(nullptr, screenDC);
      DeleteDC(memDC_);
      memDC_ = nullptr;
      return false;
    }
    SelectObject(memDC_, hDib_);
    ReleaseDC(nullptr, screenDC);
    return true;
  }

  void RenderAndUpdateWindow() {
    if (!pRenderTarget_ || !pwicBitmap_ || !dibBits_) return;

    // 绘制到 WIC bitmap via D2D render target
    pRenderTarget_->BeginDraw();

    // 清空（完全透明）
    pRenderTarget_->Clear(D2D1::ColorF(0, 0.0f));

    // 半透明蒙层：用黑色 brush，比直接 Clear 更易控制
    ID2D1SolidColorBrush* pBlack = nullptr;
    pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(0, 0.6f), &pBlack);
    D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, (FLOAT)screenW_, (FLOAT)screenH_);
    pRenderTarget_->FillRectangle(&full, pBlack);

    ID2D1SolidColorBrush* pBorder = nullptr;
    pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.75f, 1.0f, 1.0f), &pBorder);

    ID2D1SolidColorBrush* pFill = nullptr;
    pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.75f, 1.0f, 0.12f), &pFill);

    ID2D1SolidColorBrush* pGuide = nullptr;
    pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.25f), &pGuide);

    ID2D1SolidColorBrush* pTextBrush = nullptr;
    pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pTextBrush);

    ID2D1SolidColorBrush* pBgBrush = nullptr;
    pRenderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.05f, 0.58f), &pBgBrush);

    POINT cursorPt = currentPoint_;
    pRenderTarget_->DrawLine(
        D2D1::Point2F(0.0f, static_cast<float>(cursorPt.y)),
        D2D1::Point2F(static_cast<float>(screenW_), static_cast<float>(cursorPt.y)), pGuide, 1.0f);
    pRenderTarget_->DrawLine(
        D2D1::Point2F(static_cast<float>(cursorPt.x), 0.0f),
        D2D1::Point2F(static_cast<float>(cursorPt.x), static_cast<float>(screenH_)), pGuide, 1.0f);

    // 画选区
    RECT sel = GetActiveSelection();
    D2D1_RECT_F r
        = D2D1::RectF((FLOAT)sel.left, (FLOAT)sel.top, (FLOAT)sel.right, (FLOAT)sel.bottom);

    int w = std::max(0L, sel.right - sel.left);
    int h = std::max(0L, sel.bottom - sel.top);
    const bool hasRect = w > 0 && h > 0;

    if (hasRect) {
      pRenderTarget_->FillRectangle(r, pFill);
      pRenderTarget_->DrawRectangle(r, pBorder, 2.0f);

      const float handle = 8.0f;
      pRenderTarget_->FillRectangle(D2D1::RectF(r.left - handle / 2, r.top - handle / 2,
                                                r.left + handle / 2, r.top + handle / 2),
                                    pBorder);
      pRenderTarget_->FillRectangle(D2D1::RectF(r.right - handle / 2, r.top - handle / 2,
                                                r.right + handle / 2, r.top + handle / 2),
                                    pBorder);
      pRenderTarget_->FillRectangle(D2D1::RectF(r.left - handle / 2, r.bottom - handle / 2,
                                                r.left + handle / 2, r.bottom + handle / 2),
                                    pBorder);
      pRenderTarget_->FillRectangle(D2D1::RectF(r.right - handle / 2, r.bottom - handle / 2,
                                                r.right + handle / 2, r.bottom + handle / 2),
                                    pBorder);
    }

    std::wstring text;
    if (hasRect) {
      const int absLeft = originX_ + sel.left;
      const int absTop = originY_ + sel.top;
      const int absRight = originX_ + sel.right;
      const int absBottom = originY_ + sel.bottom;
      const int centerX = originX_ + (sel.left + sel.right) / 2;
      const int centerY = originY_ + (sel.top + sel.bottom) / 2;
      wchar_t info[320];
      swprintf_s(info,
                 L"Selection\n"
                 L"LT: (%d, %d)  RB: (%d, %d)\n"
                 L"Size: %d x %d   Center: (%d, %d)\n"
                 L"Drag inside box to move   Enter/Double Click to confirm\n"
                 L"Click outside to reselect   Esc/Right Click to cancel",
                 absLeft, absTop, absRight, absBottom, w, h, centerX, centerY);
      text = info;
    } else {
      const int absX = originX_ + cursorPt.x;
      const int absY = originY_ + cursorPt.y;
      wchar_t info[192];
      swprintf_s(info,
                 L"Region Selection\n"
                 L"Cursor: (%d, %d)\n"
                 L"Left drag to select   Esc/Right Click to cancel",
                 absX, absY);
      text = info;
    }

    if (pTextFormat_ && !text.empty()) {
      D2D1_RECT_F textRect = GetInfoRect(sel, hasRect);

      pRenderTarget_->FillRoundedRectangle(D2D1::RoundedRect(textRect, 8.0f, 8.0f), pBgBrush);
      pRenderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), pTextFormat_,
                                textRect, pTextBrush);
    }

    // 结束绘制
    HRESULT hr = pRenderTarget_->EndDraw();
    if (pBlack) pBlack->Release();
    if (pBorder) pBorder->Release();
    if (pFill) pFill->Release();
    if (pGuide) pGuide->Release();
    if (pTextBrush) pTextBrush->Release();
    if (pBgBrush) pBgBrush->Release();

    if (FAILED(hr)) {
      // 失败则不更新
      return;
    }

    // 把 WIC bitmap 像素复制到 DIBSection 的内存（并在复制后把选区 alpha 清零）
    // Lock WIC bitmap并得到像素缓冲
    WICRect lockRect = {0, 0, screenW_, screenH_};
    IWICBitmapLock* pLock = nullptr;
    HRESULT lk = pwicBitmap_->Lock(&lockRect, WICBitmapLockRead, &pLock);
    if (FAILED(lk) || !pLock) return;

    UINT cbBufferSize = 0;
    BYTE* pData = nullptr;
    UINT stride = 0;
    pLock->GetDataPointer(&cbBufferSize, &pData);
    pLock->GetStride(&stride);

    // dibBits_ 是 DIBSection 的 BGRA 内存，stride == screenW_*4 通常一致
    BYTE* dst = reinterpret_cast<BYTE*>(dibBits_);
    for (int y = 0; y < screenH_; ++y) {
      BYTE* srcRow = pData + y * stride;
      BYTE* dstRow = dst + y * screenW_ * 4;
      // 如果这一行与选区无交集则直接 memcpy
      if (!hasRect || y < sel.top || y >= sel.bottom) {
        memcpy(dstRow, srcRow, screenW_ * 4);
      } else {
        // 部分覆盖：复制左段、清空选区段、复制右段
        int left = sel.left;
        int right = sel.right;
        if (left > 0) memcpy(dstRow, srcRow, left * 4);
        // 选区段保留极低 alpha，避免 layered window 命中测试直接穿透。
        int w = std::max(0, right - left);
        if (w > 0) {
          BYTE* selRow = dstRow + left * 4;
          for (int x = 0; x < w; ++x) {
            selRow[x * 4 + 0] = 0x00;  // B
            selRow[x * 4 + 1] = 0x00;  // G
            selRow[x * 4 + 2] = 0x00;  // R
            selRow[x * 4 + 3] = 0x01;  // A
          }
        }
        if (right < screenW_) {
          memcpy(dstRow + right * 4, srcRow + right * 4, (screenW_ - right) * 4);
        }
      }
    }

    pLock->Release();

    // 更新分层窗口
    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {screenW_, screenH_};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    HDC hScreenDC = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, hScreenDC, nullptr, &sizeWnd, memDC_, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, hScreenDC);
  }
};
