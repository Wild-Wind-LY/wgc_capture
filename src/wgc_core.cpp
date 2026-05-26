#include "wgc_core.hpp"

#include <dwmapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "gpu_encoder.hpp"
#define QOI_IMPLEMENTATION
#include "qoi.h"
#include "rgba_nv12_converter.hpp"

static bool IsWindowsGraphicsCaptureSupported() {
  try {
    return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
  } catch (...) {
    return false;
  }
}

static std::string hstr2str(const winrt::hstring& hstr) {
  const wchar_t* wstr = hstr.c_str();
  int wlen = static_cast<int>(wcslen(wstr));
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
  if (len == 0) return {};

  std::string result(len, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, result.data(), len, nullptr, nullptr);
  return result;
}

std::string WgcCore::GetWindowsVersionString() {
  using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

  HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
  if (hMod) {
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(hMod, "RtlGetVersion"));
    if (rtlGetVersion) {
      OSVERSIONINFOEXW osInfo = {};
      osInfo.dwOSVersionInfoSize = sizeof(osInfo);
      if (rtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osInfo)) == 0) {
        std::ostringstream oss;
        oss << osInfo.dwMajorVersion << "." << osInfo.dwMinorVersion << "." << osInfo.dwBuildNumber;
        return oss.str();
      }
    }
  }
  return "Unknown";
}

WgcCore::WgcCore() {
  if (!IsWindowsGraphicsCaptureSupported()) {
    throw std::runtime_error("Windows Graphics Capture not supported on this OS version.");
  }
  // SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // 明确指定多线程模型
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  CreateDevice();

  // 检查device支持的类型
  CheckVideoFormatSupport();

  // 创建转换器
  // m_converter = std::make_unique<RgbaNv12ConverterD3D11>(m_d3dDevice);

  // 创建编码器
  // m_gpuEncoder = std::make_unique<GpuEncoder>(m_d3dDevice);
  // m_gpuEncoder->Start();

  // 启动编码线程
  m_encodingThreadRunning = true;
  m_encodingThread = std::thread([this]() {
    while (m_encodingThreadRunning) {
      RawFrameData rawFrame{};
      {
        std::unique_lock lock(m_rawQueueMutex);
        m_rawQueueCV.wait(lock,
                          [&]() { return !m_rawFrameQueue.empty() || !m_encodingThreadRunning; });
        if (!m_encodingThreadRunning) break;

        if (!m_rawFrameQueue.pop(rawFrame)) {
          continue;
        }
      }
#if 0
      // 计时 ConvertRGBAtoNV12
      // auto start_rgba_to_nv12 = std::chrono::high_resolution_clock::now();
      // winrt::com_ptr<ID3D11Texture2D> nv12;
      // m_converter->ConvertRGBAtoNV12(rawFrame.texture, nv12);
      // auto end_rgba_to_nv12 = std::chrono::high_resolution_clock::now();
      // auto duration_rgba_to_nv12 =
      //     std::chrono::duration_cast<std::chrono::microseconds>(
      //         end_rgba_to_nv12 - start_rgba_to_nv12)
      //         .count();
      // std::cout << "[RGBAtoNV12] Duration: " << std::dec
      //           << duration_rgba_to_nv12 << "us\n";

      // m_gpuEncoder->EnqueueFrame(nv12);

      // 计时 ConvertNV12ToRGBA
      // auto start_nv12_to_rgba = std::chrono::high_resolution_clock::now();
      // winrt::com_ptr<ID3D11Texture2D> rgba;
      // m_converter->ConvertNV12ToRGBA(nv12, rgba);
      // auto end_nv12_to_rgba = std::chrono::high_resolution_clock::now();
      // auto duration_nv12_to_rgba =
      //     std::chrono::duration_cast<std::chrono::microseconds>(
      //         end_nv12_to_rgba - start_nv12_to_rgba)
      //         .count();
      // std::cout << "[NV12ToRGBA] Duration: " << std::dec
      //           << duration_nv12_to_rgba << "us\n";
#endif

      // 线程局部变量，线程安全独立缓存
      thread_local StagingTextureCache stagingCache;

      // Keep the immediate context on one worker thread during GPU readback.
      auto* stagingTex = stagingCache.GetOrCreate(rawFrame.desc, m_d3dDevice.get());

      m_d3dContext->CopyResource(stagingTex, rawFrame.texture.get());

      D3D11_MAPPED_SUBRESOURCE mapped{};
      HRESULT hr = m_d3dContext->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(hr)) continue;

      std::vector<uint8_t> rgbaData;
      rgbaData.resize(rawFrame.desc.Width * rawFrame.desc.Height * 4);
      const uint32_t expectedPitch = rawFrame.desc.Width * 4;
      if (mapped.RowPitch == expectedPitch) {
        std::memcpy(rgbaData.data(), mapped.pData, rawFrame.desc.Height * expectedPitch);
      } else {
        for (uint32_t y = 0; y < rawFrame.desc.Height; ++y) {
          const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
          uint8_t* dst = rgbaData.data() + y * expectedPitch;
          std::memcpy(dst, src, expectedPitch);
        }
      }

      m_d3dContext->Unmap(stagingTex, 0);
      m_readbackFrames.fetch_add(1, std::memory_order_relaxed);

#if 0
      std::vector<uint8_t> qoiBuffer{};
      if (!EncodeQoiToBuffer(rgbaData.data(), rawFrame.desc, qoiBuffer)) {
        std::cout << "Async qoi encoding failed." << std::endl;
      } else {
        std::lock_guard<std::mutex> lock(m_encodingMutex);
        if (m_encodedFrame) {
          m_outputDroppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        m_encodedFrame =
            EncodedFrame{rawFrame.frameIndex, rawFrame.desc, std::move(qoiBuffer)};
      }
#else
      std::vector<uint8_t> rawBuffer{};
      rawBuffer = std::move(rgbaData);  // 直接拿到 CPU 内存里的 RGBA 数据
      if (rawBuffer.empty()) {
        std::cout << "Async raw frame copy failed." << std::endl;
      } else {
        std::lock_guard<std::mutex> lock(m_encodingMutex);
        if (m_encodedFrame) {
          m_outputDroppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        m_encodedFrame = EncodedFrame{rawFrame.frameIndex, rawFrame.desc, std::move(rawBuffer)};
      }
#endif
    }
  });
}

WgcCore::~WgcCore() {
  // 停止捕获
  Stop();

  // m_gpuEncoder->Stop();

  // 清理 WinRT 资源（智能指针会自动释放，显式清空可以更明确）
  m_session = nullptr;
  m_framePool = nullptr;
  m_item = nullptr;

  // 停止编码线程
  m_encodingThreadRunning = false;
  m_rawQueueCV.notify_all();
  if (m_encodingThread.joinable()) {
    m_encodingThread.join();
  }

  // D3D 设备和上下文智能指针会自动释放
}

void WgcCore::CreateDevice() {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  D3D_FEATURE_LEVEL featureLevels[]
      = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};

  D3D_FEATURE_LEVEL featureLevel;

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, featureLevels,
                                 ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, m_d3dDevice.put(),
                                 &featureLevel, m_d3dContext.put());
  CHECK_HR(hr, "Failed to create D3D11 device.");

  // 转 IDXGIDevice
  winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();

  // 由 dxgiDevice 创建 WinRT Direct3DDevice
  winrt::com_ptr<::IInspectable> inspectableDevice;
  hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectableDevice.put());
  if (FAILED(hr)) throw std::runtime_error("CreateDirect3D11DeviceFromDXGIDevice failed");

  // 转为 IDirect3DDevice
  m_winrtDevice
      = inspectableDevice.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

void WgcCore::CreateCaptureItem(HWND hwnd) {
  if (!hwnd) throw std::invalid_argument("Invalid HWND.");

  auto factory
      = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                      IGraphicsCaptureItemInterop>();

  if (!factory) {
    throw std::runtime_error("Failed to get IGraphicsCaptureItemInterop factory.");
  }

  // 引入ABI命名空间别名，部分项目中需使用
  using namespace ABI::Windows::Graphics::Capture;
  HRESULT hr = factory->CreateForWindow(hwnd, __uuidof(IGraphicsCaptureItem),
                                        reinterpret_cast<void**>(winrt::put_abi(m_item)));
  CHECK_HR(hr, "Failed to create capture item.");

  if (!m_item) {
    throw std::runtime_error("Capture item is null after creation.");
  }
}

void WgcCore::CreateCaptureItem(HMONITOR hmonitor) {
  if (!hmonitor) throw std::invalid_argument("Invalid hmonitor.");

  auto factory
      = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                      IGraphicsCaptureItemInterop>();

  if (!factory) {
    throw std::runtime_error("Failed to get IGraphicsCaptureItemInterop factory.");
  }

  // 引入ABI命名空间别名，部分项目中需使用
  using namespace ABI::Windows::Graphics::Capture;
  HRESULT hr = factory->CreateForMonitor(hmonitor, __uuidof(IGraphicsCaptureItem),
                                         reinterpret_cast<void**>(winrt::put_abi(m_item)));
  CHECK_HR(hr, "Failed to create capture item.");

  if (!m_item) {
    throw std::runtime_error("Capture item is null after creation.");
  }
}

// bool WgcCore::IsWindowCapturable(HWND hwnd) {
//   if (!::IsWindow(hwnd)) return false;
//   if (!::IsWindowVisible(hwnd)) return false;
//   if (::IsIconic(hwnd)) return false;
//   RECT rc;
//   if (!::GetClientRect(hwnd, &rc)) return false;
//   if ((rc.right - rc.left) == 0 || (rc.bottom - rc.top) == 0) return false;
//   return true;
// }

bool WgcCore::IsWindowCapturable(HWND hwnd) {
  if (!::IsWindow(hwnd)) return false;
  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
    if (cloaked) return false;
  }
  if (::IsIconic(hwnd)) return false;

  return true;
}

bool WgcCore::CheckWgcSupported() {
#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0A00)
  constexpr bool sdk_supports_win10 = true;
#else
  constexpr bool sdk_supports_win10 = false;
#endif

#if defined(PROJECT_SDK_MAJOR) && defined(PROJECT_SDK_BUILD)
  constexpr bool sdk_supports_win10_build
      = (PROJECT_SDK_MAJOR > 10) || (PROJECT_SDK_MAJOR == 10 && PROJECT_SDK_BUILD >= 19041);
#else
  constexpr bool sdk_supports_win10_build = true;  // 默认通过
#endif

  if (!sdk_supports_win10) {
    std::cerr << "WGC not supported: SDK does not target Windows 10." << std::endl;
    return false;
  }
  if (!sdk_supports_win10_build) {
    std::cerr << "WGC not supported: SDK build < 19041." << std::endl;
    return false;
  }
  // if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::
  //         IsSupported()) {
  //   std::cerr << "WGC not supported on this system." << std::endl;
  //   return false;
  // }

  return true;
}

void WgcCore::CheckVideoFormatSupport() const {
  struct FormatInfo {
    DXGI_FORMAT format;
    const char* name;
  };

  std::vector<FormatInfo> formats = {
      {DXGI_FORMAT_NV12, "NV12"},
      {DXGI_FORMAT_YUY2, "YUY2"},
  };

  for (const auto& fmt : formats) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT support1 = {fmt.format};
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2 = {fmt.format};

    bool hasSupport1 = SUCCEEDED(m_d3dDevice->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT,
                                                                  &support1, sizeof(support1)));

    std::cout << fmt.name << ":\n";

    if (!hasSupport1) {
      std::cout << "  - D3D11_FORMAT_SUPPORT1 not supported.\n";
      continue;
    }

    if ((support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0)
      std::cout << "  - Texture2D Supported\n";
    if ((support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0)
      std::cout << "  - Shader Sample Supported\n";
    if ((support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) != 0)
      std::cout << "  - Video Processor Output Supported\n";
    if ((support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) != 0)
      std::cout << "  - Video Processor Input Supported\n";
    if ((support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_VIDEO_ENCODER) != 0)
      std::cout << "  - Video Encoder Supported\n";

    std::cout << std::endl;
  }
}

bool WgcCore::Initialize(uintptr_t h_val, CaptureType type) {
  if (!CheckWgcSupported()) return false;

  try {
    switch (type) {
      case CaptureType::Window: {
        HWND hwnd = reinterpret_cast<HWND>(h_val);

        if (!IsWindowCapturable(hwnd)) {
          std::cerr << "窗口当前不可捕获!\n";
          return false;
        }
        // 创建 DispatcherQueueController 保证事件回调能触发
        if (!m_dispatcherQueueController) {
          m_dispatcherQueueController
              = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
          if (!m_dispatcherQueueController) {
            std::cerr << "Failed to create DispatcherQueueController." << std::endl;
            return false;
          }
        }

        CreateCaptureItem(hwnd);
        m_lastSize = m_item.Size();

        // 增加帧池大小，从1增加到2以提高帧缓存和性能
        m_framePool
            = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::
                    //  DirectXPixelFormat::B8G8R8A8UIntNormalized,
                DirectXPixelFormat::R8G8B8A8UIntNormalized,
                2, m_lastSize);

        m_frameArrivedRevoker
            = m_framePool.FrameArrived(winrt::auto_revoke, {this, &WgcCore::OnFrameArrived});

        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(false);

#if WGC_ADVANCED_API
        m_session.IsBorderRequired(true);
        auto interval = m_session.MinUpdateInterval();
        double intervalMs = interval.count() / 10000.0;
        std::cout << "MinUpdateInterval = " << intervalMs << " ms\n";
#else
        std::cout << "高级 WGC API 不支持，使用默认设置\n";
#endif
        return true;

      } break;
      case CaptureType::Monitor: {
        HMONITOR hmonitor = reinterpret_cast<HMONITOR>(h_val);

        // 创建 DispatcherQueueController 保证事件回调能触发
        if (!m_dispatcherQueueController) {
          m_dispatcherQueueController
              = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
          if (!m_dispatcherQueueController) {
            std::cerr << "Failed to create DispatcherQueueController." << std::endl;
            return false;
          }
        }
        CreateCaptureItem(hmonitor);
        m_lastSize = m_item.Size();

        // 增加帧池大小，从1增加到2以提高帧缓存和性能
        m_framePool
            = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::
                    //  DirectXPixelFormat::B8G8R8A8UIntNormalized,
                DirectXPixelFormat::R8G8B8A8UIntNormalized,
                2, m_lastSize);

        m_frameArrivedRevoker
            = m_framePool.FrameArrived(winrt::auto_revoke, {this, &WgcCore::OnFrameArrived});

        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(false);

#if WGC_ADVANCED_API
        m_session.IsBorderRequired(true);
        auto interval = m_session.MinUpdateInterval();
        double intervalMs = interval.count() / 10000.0;
        std::cout << "MinUpdateInterval = " << intervalMs << " ms\n";
#else
        std::cout << "高级 WGC API 不支持，使用默认设置\n";
#endif

        return true;

      } break;
      default:
        throw std::runtime_error("Initialize failed. CaptureType is error.");
        break;
    }

  } catch (const std::exception& ex) {
    std::cerr << "Initialize capture error: " << ex.what() << std::endl;
  }

  return false;
}

void WgcCore::Start() {
  if (m_started.exchange(true)) return;
  if (!m_session) throw std::runtime_error("Capture session not initialized.");
  m_session.StartCapture();
}

void WgcCore::Stop() {
  if (!m_started.exchange(false)) return;

  if (m_frameArrivedRevoker) m_frameArrivedRevoker.revoke();

  if (m_session) m_session.Close();
  if (m_framePool) m_framePool.Close();

  m_session = nullptr;
  m_framePool = nullptr;
  m_item = nullptr;
  m_dispatcherQueueController = nullptr;
}

auto WgcCore::GetEncodedFrame() -> std::optional<EncodedFrame> {
  std::unique_lock lock(m_encodingMutex);
  if (!m_encodedFrame) return std::nullopt;

  EncodedFrame frame = std::move(*m_encodedFrame);
  m_encodedFrame.reset();

  return frame;
}

CaptureStats WgcCore::GetStats() {
  std::vector<double> intervals;
  {
    std::lock_guard<std::mutex> lock(m_captureTimingMutex);
    intervals.swap(m_captureIntervalsMs);
  }

  CaptureStats stats{
      m_capturedFrames.load(std::memory_order_relaxed),
      m_duplicateFrames.load(std::memory_order_relaxed),
      m_readbackFrames.load(std::memory_order_relaxed),
      m_rawDroppedFrames.load(std::memory_order_relaxed),
      m_outputDroppedFrames.load(std::memory_order_relaxed),
  };

  if (!intervals.empty()) {
    std::sort(intervals.begin(), intervals.end());
    const size_t p95Index =
        static_cast<size_t>(std::ceil(intervals.size() * 0.95)) - 1;
    stats.captureIntervalMaxMs = intervals.back();
    stats.captureIntervalP95Ms = intervals[p95Index];
    stats.captureIntervalSamples = intervals.size();
  }

  return stats;
}

auto WgcCore::DecodeQoiToFrame(const EncodedFrame& frame) -> std::optional<FrameData> {
  qoi_desc desc{};
  // 4 表示 RGBA
  void* decoded = qoi_decode(frame.data.data(), frame.data.size(), &desc, 4);
  if (!decoded) {
    return std::nullopt;
  }

  size_t dataSize = static_cast<size_t>(desc.width) * desc.height * desc.channels;
  std::vector<uint8_t> rgbaData(dataSize);
  std::memcpy(rgbaData.data(), decoded, dataSize);
  free(decoded);

  FrameData rawframe;
  rawframe.rgbaData = std::move(rgbaData);
  rawframe.desc.Width = desc.width;
  rawframe.desc.Height = desc.height;
  rawframe.frameIndex = frame.frameIndex;

  return rawframe;
}

void WgcCore::OnFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const&) {
  if (!m_started.load()) return;
  auto newSize = false;

  try {
    // auto frame = sender.TryGetNextFrame();
    while (auto frame = m_framePool.TryGetNextFrame()) {
      if (!frame) return;

      const auto captureTimestamp = frame.SystemRelativeTime();
      {
        std::lock_guard<std::mutex> lock(m_captureTimingMutex);
        if (m_lastCaptureTimestamp.count() != 0 &&
            captureTimestamp == m_lastCaptureTimestamp) {
          m_duplicateFrames.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (m_lastCaptureTimestamp.count() != 0) {
          m_captureIntervalsMs.push_back(
              std::chrono::duration<double, std::milli>(
                  captureTimestamp - m_lastCaptureTimestamp)
                  .count());
        }
        m_lastCaptureTimestamp = captureTimestamp;
      }
      m_capturedFrames.fetch_add(1, std::memory_order_relaxed);

      auto size = frame.ContentSize();

      // UINT evenWidth = size.Width & ~15;
      // UINT evenHeight = size.Height & ~15;
      // if (evenWidth == 0 || evenHeight == 0) return;
      // bool sizeChanged =
      //     (evenWidth != m_lastSize.Width) || (evenHeight !=
      //     m_lastSize.Height);
      bool sizeChanged = (size.Width != m_lastSize.Width) || (size.Height != m_lastSize.Height);

      if (sizeChanged) {
        newSize = true;
        m_lastSize.Width = size.Width;
        m_lastSize.Height = size.Height;
      }

      auto surface = frame.Surface();
      auto frameTexture = GetDXGIInterface<ID3D11Texture2D>(surface);

      D3D11_TEXTURE2D_DESC desc{};
      frameTexture->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = 0;
      // desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;

#if 0
      if (size.Width != evenWidth || size.Height != evenHeight) {
        // 创建裁剪纹理
        D3D11_TEXTURE2D_DESC croppedDesc;
        frameTexture->GetDesc(&croppedDesc);
        croppedDesc.Width = evenWidth;
        croppedDesc.Height = evenHeight;

        winrt::com_ptr<ID3D11Texture2D> croppedTexture;
        HRESULT hr = m_d3dDevice->CreateTexture2D(&croppedDesc, nullptr,
                                                  croppedTexture.put());
        if (FAILED(hr)) {
          std::cerr << "Create staging texture failed. HRESULT=0x" << std::hex
                    << hr << std::endl;
          return;
        }

        D3D11_BOX srcBox = {};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = std::min(evenWidth, desc.Width);
        srcBox.bottom = std::min(evenHeight, desc.Height);
        srcBox.back = 1;
        m_d3dContext->CopySubresourceRegion(croppedTexture.get(), 0, 0, 0, 0,
                                            frameTexture.get(), 0, &srcBox);

        frameTexture = croppedTexture;
        desc = croppedDesc;
      }
#endif
      // RawFrameData rawFrame{};
      // rawFrame.frameIndex = m_frameIndex++;
      // rawFrame.desc = desc;
      // rawFrame.texture = frameTexture;

      // D3D11_TEXTURE2D_DESC tempDesc;
      // rawFrame.texture->GetDesc(&tempDesc);
      // std::cout << std::dec << "tempDesc size: " << tempDesc.Width << "x"
      //           << tempDesc.Height << std::endl
      //           << std::endl;

      if (m_rawFrameQueue.push({m_frameIndex++, desc, frameTexture})) {
        m_rawQueueCV.notify_one();
      } else {
        m_rawDroppedFrames.fetch_add(1, std::memory_order_relaxed);
      }

      if (newSize) {
        m_framePool.Recreate(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R8G8B8A8UIntNormalized, 2,
            m_lastSize);
      }
    }

  } catch (const std::exception& e) {
    std::cerr << "OnFrameArrived exception: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "OnFrameArrived unknown exception." << std::endl;
  }
}

#if 0
// 优化的PNG编码，预分配内存提升性能
bool WgcCore::EncodePngToBuffer(uint8_t* rgbaData, uint32_t width,
                                   uint32_t height,
                                   std::vector<uint8_t>& outPng) {
  png_structp png_ptr =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr) return false;
  // png_set_compression_level(png_ptr, 0);  // 设置压缩等级
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr, nullptr);
    return false;
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return false;
  }

  // 预估大小预分配，减少多次内存分配
  outPng.reserve(width * height * 5);

  png_set_write_fn(
      png_ptr, &outPng,
      [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto vec = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png_ptr));
        vec->insert(vec->end(), data, data + length);
      },
      nullptr);

  png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png_ptr, info_ptr);

  std::vector<uint8_t> row(width * 4);

  // BGRA -> RGBA转换 (简化写法)
  // for (uint32_t y = 0; y < height; ++y) {
  //   auto* srcRow = bgraData + y * width * 4;
  //   for (uint32_t x = 0; x < width; ++x) {
  //     row[x * 4 + 0] = srcRow[x * 4 + 2];  // R
  //     row[x * 4 + 1] = srcRow[x * 4 + 1];  // G
  //     row[x * 4 + 2] = srcRow[x * 4 + 0];  // B
  //     row[x * 4 + 3] = srcRow[x * 4 + 3];  // A
  //   }
  //   png_write_row(png_ptr, row.data());
  // }
  // 直接写入每一行（不需要格式转换）
  for (uint32_t y = 0; y < height; ++y) {
    auto* srcRow = rgbaData + y * width * 4;
    std::memcpy(row.data(), srcRow, width * 4);
    png_write_row(png_ptr, row.data());
  }

  png_write_end(png_ptr, nullptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);

  return true;
}
#endif

bool WgcCore::EncodeQoiToBuffer(uint8_t* rgbaData, const D3D11_TEXTURE2D_DESC& texDesc,
                                std::vector<uint8_t>& outQoi) {
  qoi_desc desc;
  desc.width = texDesc.Width;
  desc.height = texDesc.Height;
  // desc.channels = (texDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) ? 4 : 3;
  // desc.colorspace = (texDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
  //                       ? QOI_SRGB
  //                       : QOI_LINEAR;
  desc.channels = 4;
  desc.colorspace = QOI_SRGB;

  int outLen = 0;
  void* qoiDataRaw = qoi_encode(rgbaData, &desc, &outLen);
  if (!qoiDataRaw || outLen <= 0) {
    std::cout << "QOI encoding failed. Output length = " << outLen << "\n";
    return false;
  }

  std::unique_ptr<uint8_t, decltype(&free)> qoiData(static_cast<uint8_t*>(qoiDataRaw), &free);

  outQoi.assign(qoiData.get(), qoiData.get() + outLen);
  return true;
}

#if 0
bool WgcCore::EncodeQoiToBuffer(uint8_t* rgbaData, uint32_t width,
                                   uint32_t height,
                                   std::vector<uint8_t>& outQoi) {
  constexpr int kBlockRows = 32; // 每块处理64行（可调）
  int numBlocks = (height + kBlockRows - 1) / kBlockRows;

  std::vector<uint8_t> fullImage(width * height * 4);
  std::mutex copyMutex;

  std::vector<std::future<void>> tasks;
  for (int block = 0; block < numBlocks; ++block) {
    int rowStart = block * kBlockRows;
    int rowEnd = std::min<int>(rowStart + kBlockRows, height);

    tasks.emplace_back(MyThreadPool::threadPool.submit_task([=, &fullImage, &copyMutex]() {
      int numRows = rowEnd - rowStart;
      int stride = width * 4;

      const uint8_t* src = rgbaData + rowStart * stride;
      uint8_t* dst = fullImage.data() + rowStart * stride;

      std::memcpy(dst, src, numRows * stride);
      // 如果还需要颜色空间转换或其他预处理，可在这里处理
    }));
  }

  // 等待所有块复制完成
  for (auto& task : tasks) task.get();

  // 主线程执行 QOI 编码
  qoi_desc desc;
  desc.width = width;
  desc.height = height;
  desc.channels = 4;
  desc.colorspace = QOI_SRGB;

  int outLen = 0;
  void* qoiDataRaw = qoi_encode(fullImage.data(), &desc, &outLen);
  if (!qoiDataRaw || outLen <= 0) {
    std::cout << "QOI encoding failed. Output length = " << outLen << "\n";
    return false;
  }

  std::unique_ptr<uint8_t, decltype(&free)> qoiData(
      static_cast<uint8_t*>(qoiDataRaw), &free);

  outQoi.assign(qoiData.get(), qoiData.get() + outLen);
  return true;
}
#endif

#if 0
    if (!m_stagingTexture || desc.Width != m_stagingDesc.Width ||
        desc.Height != m_stagingDesc.Height) {
      m_stagingDesc = desc;
      m_stagingDesc.Usage = D3D11_USAGE_STAGING;
      m_stagingDesc.BindFlags = 0;
      m_stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      m_stagingDesc.MiscFlags = 0;

      HRESULT hr = m_d3dDevice->CreateTexture2D(&m_stagingDesc, nullptr,
                                                m_stagingTexture.put());
      if (FAILED(hr)) {
        std::cerr << "Create staging texture failed. HRESULT=0x" << std::hex
                  << hr << std::endl;
        return;
      }
    }
    // 在 Map 前记录时间
    // auto start = std::chrono::high_resolution_clock::now();
    m_d3dContext->CopyResource(m_stagingTexture.get(), frameTexture.get());
    // auto end = std::chrono::high_resolution_clock::now();
    // auto duration_us =
    //     std::chrono::duration_cast<std::chrono::microseconds>(end - start)
    //         .count();
    // std::cout << "[CopyResource] Duration: " << std::dec << duration_us << "
    // us"
    //           << std::endl;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, 0,
                                   &mapped);
    if (FAILED(hr)) {
      std::cerr << "Map staging texture failed. HRESULT=0x" << std::hex << hr
                << std::endl;
      return;
    }

    // 复制图像数据到连续 buffer
    const uint32_t width = desc.Width;
    const uint32_t height = desc.Height;
    const size_t rowSize = width * 4;
    std::vector<uint8_t> contiguousBuffer(width * height * 4);

    for (uint32_t y = 0; y < height; ++y) {
      uint8_t* srcRow =
          reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
      uint8_t* dstRow = contiguousBuffer.data() + y * rowSize;
      memcpy(dstRow, srcRow, rowSize);
    }

    m_d3dContext->Unmap(m_stagingTexture.get(), 0);
#endif

// 自动检测并设置为最高级别的 DPI 感知模式
void dpi_helper::EnablePerMonitorV2DpiAwareness() {
  using SetProcessDpiAwarenessContext_t = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);

  HMODULE user32 = LoadLibraryW(L"user32.dll");
  if (!user32) {
    std::cerr << "[DPI] Failed to load user32.dll\n";
    return;
  }

  auto set_dpi_ctx = reinterpret_cast<SetProcessDpiAwarenessContext_t>(
      GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (set_dpi_ctx) {
    // 最理想的模式：每个显示器独立 DPI 感知 V2
    if (set_dpi_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      std::cout << "[DPI] Enabled Per-Monitor V2 DPI awareness\n";
      FreeLibrary(user32);
      return;
    }
  }

  // 如果失败，尝试 Windows 8.1 的 API
  using SetProcessDpiAwareness_t = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);
  HMODULE shcore = LoadLibraryW(L"Shcore.dll");
  if (shcore) {
    auto set_dpi_awareness = reinterpret_cast<SetProcessDpiAwareness_t>(
        GetProcAddress(shcore, "SetProcessDpiAwareness"));
    if (set_dpi_awareness) {
      if (SUCCEEDED(set_dpi_awareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
        std::cout << "[DPI] Enabled Per-Monitor DPI awareness (fallback)\n";
        FreeLibrary(shcore);
        FreeLibrary(user32);
        return;
      }
    }
    FreeLibrary(shcore);
  }

  // 最后再试 Windows Vista 的 SetProcessDPIAware
  auto set_dpi_aware
      = reinterpret_cast<BOOL(WINAPI*)(void)>(GetProcAddress(user32, "SetProcessDPIAware"));
  if (set_dpi_aware && set_dpi_aware()) {
    std::cout << "[DPI] Enabled System DPI awareness (legacy)\n";
  } else {
    std::cerr << "[DPI] Failed to enable DPI awareness\n";
  }

  FreeLibrary(user32);
}
