#pragma once

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "Winmm.lib")

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <winrt/base.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "lock_free_queue.hpp"

#define CHECK(x, err)              \
  if (!(x)) {                      \
    std::cerr << err << std::endl; \
    return;                        \
  }
#define CHECK_HR(x, err)           \
  if (FAILED(x)) {                 \
    std::cerr << err << std::endl; \
    return;                        \
  }

class GpuEncoder {
public:
  explicit GpuEncoder(winrt::com_ptr<ID3D11Device> device);
  ~GpuEncoder();

  void Start();
  void Stop();

  void EnqueueFrame(winrt::com_ptr<ID3D11Texture2D> frame);
  void ResetEncoder();

private:
  void EncodeLoop();
  void Init();
  void Cleanup();

  void WriteSampleToFile(IMFSample* sample, std::ofstream& file);

  winrt::com_ptr<ID3D11Texture2D> CreateBlackNV12Texture(int width, int height);

  std::atomic<bool> running_{false};
  std::thread worker_;

  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<IMFDXGIDeviceManager> deviceManager_;
  winrt::com_ptr<ID3D11Texture2D> last_texture_;

  winrt::com_ptr<IMFTransform> transform_;
  winrt::com_ptr<IMFAttributes> attributes_;
  winrt::com_ptr<IMFMediaEventGenerator> eventGen_;
  DWORD inputStreamID_;
  DWORD outputStreamID_;

  LockFreeFrameQueue<winrt::com_ptr<ID3D11Texture2D>, 32> m_surfaceQueue;

  std::atomic_bool encoding = false;

  UINT currentWidth{1280};
  UINT currentHeight{720};
};
