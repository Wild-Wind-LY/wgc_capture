#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <ShellScalingAPI.h>
#include <Unknwn.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <vector>

#include "lock_free_queue.hpp"

constexpr size_t kMaxEncodingQueueSize = 2;
constexpr size_t kMaxRawFrameQueueSize = 2;

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID riid, void** ppvObject) = 0;
};

template <typename T>
winrt::com_ptr<T> GetDXGIInterface(winrt::Windows::Foundation::IInspectable const& obj) {
  auto access = obj.as<IDirect3DDxgiInterfaceAccess>();

  winrt::com_ptr<T> result;
  winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));

  return result;
}

struct FrameData {          // 帧数据
  uint64_t frameIndex = 0;  // 帧序号
  D3D11_TEXTURE2D_DESC desc;
  std::vector<uint8_t> rgbaData;
};

struct EncodedFrame {   // 编码后数据
  uint64_t frameIndex;  // 帧序号
  D3D11_TEXTURE2D_DESC desc;
  std::vector<uint8_t> data;

  bool operator<(const EncodedFrame& other) const { return frameIndex > other.frameIndex; }
};

class WgcCore {
  struct RawFrameData {       // 原始帧数据
    uint64_t frameIndex = 0;  // 帧序号
    D3D11_TEXTURE2D_DESC desc;
    winrt::com_ptr<ID3D11Texture2D> texture;  // GPU纹理
  };

  class StagingTextureCache {  // 暂存纹理缓存类
    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    D3D11_TEXTURE2D_DESC cachedDesc{};

    inline bool IsDescEqual(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
      return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format;
    }

  public:
    ID3D11Texture2D* GetOrCreate(const D3D11_TEXTURE2D_DESC& desc, ID3D11Device* device) {
      if (!stagingTexture || !IsDescEqual(desc, cachedDesc)) {
        cachedDesc = desc;
        cachedDesc.Usage = D3D11_USAGE_STAGING;
        cachedDesc.BindFlags = 0;
        cachedDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        cachedDesc.MiscFlags = 0;
        HRESULT hr = device->CreateTexture2D(&cachedDesc, nullptr, stagingTexture.put());
        if (FAILED(hr)) {
          throw std::runtime_error("Failed to create staging texture");
        }
      }
      return stagingTexture.get();
    }
  };

public:
  enum class CaptureType : uint8_t {
    Window = 0,
    Monitor,
  };

  explicit WgcCore();
  virtual ~WgcCore();

  // 获取Windows的版本
  static std::string GetWindowsVersionString();

  // 初始化，绑定要捕获的窗口或屏幕句柄
  bool Initialize(uintptr_t, CaptureType);

  // 开始捕获
  void Start();

  // 停止捕获
  void Stop();

  // 消耗一帧 PNG 格式图像数据
  auto GetEncodedFrame() -> std::optional<EncodedFrame>;

  // 将qoi编码的图像数据解码为RGBA原始像素数据
  auto DecodeQoiToFrame(const EncodedFrame& qoiBuffer) -> std::optional<FrameData>;

private:
  // 捕获到新帧时的回调处理函数
  void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
                      winrt::Windows::Foundation::IInspectable const& args);

  // 初始化D3D11设备
  void CreateDevice();

  // 创建捕获用的 GraphicsCaptureItem
  void CreateCaptureItem(HWND hwnd);
  void CreateCaptureItem(HMONITOR hmonitor);

  // 窗口是否可以捕获
  bool IsWindowCapturable(HWND hwnd);

  bool CheckWgcSupported();

  // 检查设备是否支持 NV12 作为渲染目标
  void CheckVideoFormatSupport() const;

  // 使用 libpng 编码 RGBA 数据为 PNG，存入 outPng
  // bool EncodePngToBuffer(uint8_t*, uint32_t, uint32_t,
  // std::vector<uint8_t>&);

  // 使用 QOI 编码 RGBA 数据为内存中的 QOI 格式数据（vector）
  bool EncodeQoiToBuffer(uint8_t*, const D3D11_TEXTURE2D_DESC&, std::vector<uint8_t>&);

private:
  std::atomic<bool> m_started{false};
  std::mutex m_rawQueueMutex;
  std::condition_variable m_rawQueueCV;

  // std::queue<RawFrameData> m_rawFrameQueue;
  LockFreeFrameQueue<RawFrameData, kMaxRawFrameQueueSize> m_rawFrameQueue;

  std::mutex m_encodingMutex;
  std::priority_queue<EncodedFrame> m_encodedQueue;
  // LockFreeFrameQueue<EncodedFrame, kMaxEncodingQueueSize> m_encodedQueue;

  size_t m_frameIndex = 0;

  std::atomic_bool m_encodingThreadRunning{false};
  std::thread m_encodingThread;

  // std::unique_ptr<RgbaNv12ConverterD3D11> m_converter;
  // std::unique_ptr<GpuEncoder> m_gpuEncoder;

  winrt::Windows::Graphics::SizeInt32 m_lastSize{};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};

  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker
      m_frameArrivedRevoker;

  winrt::Windows::System::DispatcherQueueController m_dispatcherQueueController{nullptr};

  winrt::com_ptr<ID3D11Device> m_d3dDevice;
  winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
};

namespace dpi_helper {
  void EnablePerMonitorV2DpiAwareness();
}  // namespace dpi_helper
