#include "gpu_encoder.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <chrono>

std::string WideStringToString(const std::wstring& wstr,
                               UINT codePage = CP_UTF8) {
  if (wstr.empty()) return {};
  // 计算转换后多字节字符串所需长度
  int size_needed = ::WideCharToMultiByte(
      codePage, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
  if (size_needed == 0) {
    return {};
  }
  std::string str(size_needed, 0);
  int converted_len =
      ::WideCharToMultiByte(codePage, 0, wstr.data(), (int)wstr.size(),
                            str.data(), size_needed, nullptr, nullptr);
  if (converted_len == 0) {
    return {};
  }

  return str;
}

GpuEncoder::GpuEncoder(winrt::com_ptr<ID3D11Device> device) : device_(device) {
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) {
    throw std::runtime_error("MFStartup failed");
  }

  if (!device_) {
    throw std::invalid_argument("device is null");
  }
  device_->GetImmediateContext(context_.put());

  Init();
}

GpuEncoder::~GpuEncoder() {
  Stop();
  Cleanup();
  MFShutdown();
}

void GpuEncoder::Init() {
  // DXGI device manager
  UINT resetToken = 0;
  HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, deviceManager_.put());
  CHECK_HR(hr, "DXGI manager create failed");

  hr = deviceManager_->ResetDevice(device_.get(), resetToken);
  CHECK_HR(hr, "DXGI reset failed");

  // 创建 encoder
  UINT32 activateCount = 0;
  MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, MFVideoFormat_H264};
  UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

  IMFActivate** activateRaw;
  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &info,
                 &activateRaw, &activateCount);

  winrt::com_ptr<IMFActivate> activate;
  activate.attach(activateRaw[0]);

  hr = activate->ActivateObject(IID_PPV_ARGS(transform_.put()));
  CHECK_HR(hr, "H264 encoder create failed");

  hr = transform_->GetAttributes(attributes_.put());
  CHECK_HR(hr, "Failed to get MFT attributes");

  hr = attributes_->SetString(MFT_FRIENDLY_NAME_Attribute, L"自定义编码器123");
  CHECK_HR(hr, "Failed to set MFT name");

  wchar_t name[256] = {0};
  hr = attributes_->GetString(MFT_FRIENDLY_NAME_Attribute, name, _countof(name),
                              nullptr);
  if (SUCCEEDED(hr)) {
    std::string nameNarrow = WideStringToString(name);
    std::cout << "MFT Friendly Name: " << nameNarrow << std::endl;
  } else {
    std::cout << "MFT 没有设置名称属性 或读取失败" << std::endl;
    return;
  }

  hr = attributes_->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  CHECK_HR(hr, "Failed to unlock MFT");
  hr = attributes_->SetUINT32(MF_LOW_LATENCY, TRUE);
  CHECK_HR(hr, "Failed to set MF_LOW_LATENCY");

  // hr = attributes_->QueryInterface(eventGen_.put());
  hr = transform_->QueryInterface(IID_PPV_ARGS(eventGen_.put()));
  CHECK_HR(hr, "Failed to QI for event generator");

  hr = transform_->GetStreamIDs(1, &inputStreamID_, 1, &outputStreamID_);
  if (hr == E_NOTIMPL) {
    inputStreamID_ = 0;
    outputStreamID_ = 0;
    hr = S_OK;
  }
  CHECK_HR(hr, "Failed to get stream IDs");

  // 初始化 MFT 状态
  hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  (ULONG_PTR)deviceManager_.get());
  CHECK_HR(hr, "Failed to set D3D manager");

  // 设置输入输出格式
  winrt::com_ptr<IMFMediaType> outType;
  MFCreateMediaType(outType.put());
  outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, currentWidth,
                     currentHeight);
  // outType->SetUINT32(MF_MT_H264_SUPPORTED_RATE_CONTROL_MODES, 1);
  winrt::com_ptr<ICodecAPI> codecAPI;
  codecAPI = transform_.as<ICodecAPI>();
  VARIANT var = {};
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = 1;  // eAVEncCommonRateControlMode_CBR
  hr = codecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
  VariantClear(&var);

  outType->SetUINT32(MF_MT_INTERLACE_MODE, 2);
  outType->SetUINT32(MF_MT_AVG_BITRATE, 30000000);
  transform_->SetOutputType(0, outType.get(), 0);

  winrt::com_ptr<IMFMediaType> inputType;
  hr = MFCreateMediaType(inputType.put());
  CHECK_HR(hr, "Failed to create media type");
  for (DWORD i = 0;; i++) {
    inputType = nullptr;
    hr = transform_->GetInputAvailableType(inputStreamID_, i, inputType.put());
    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = MFSetAttributeSize(inputType.get(), MF_MT_FRAME_SIZE, currentWidth,
                            currentHeight);
    // hr = inputType->SetUINT32(MF_MT_H264_SUPPORTED_RATE_CONTROL_MODES, 1);

    hr = MFSetAttributeRatio(inputType.get(), MF_MT_FRAME_RATE, 60, 1);
    hr = transform_->SetInputType(inputStreamID_, inputType.get(), 0);
    break;
  }

  std::cout << "Supported output frame rates:\n";
  for (DWORD i = 0;; i++) {
    winrt::com_ptr<IMFMediaType> mediaType;
    hr =
        transform_->GetOutputAvailableType(outputStreamID_, i, mediaType.put());
    if (FAILED(hr)) break;  // 枚举完了

    UINT32 numerator = 0, denominator = 0;
    hr = MFGetAttributeRatio(mediaType.get(), MF_MT_FRAME_RATE, &numerator,
                             &denominator);
    if (SUCCEEDED(hr)) {
      std::cout << "Output Type " << i << ": FrameRate = " << numerator << "/"
                << denominator << " = " << (double)numerator / denominator
                << " fps\n";
    } else {
      std::cout << "Output Type " << i << ": FrameRate not set\n";
    }
  }
  std::cout << "Supported input frame rates:\n";
  for (DWORD i = 0;; i++) {
    winrt::com_ptr<IMFMediaType> mediaType;
    hr = transform_->GetInputAvailableType(inputStreamID_, i, mediaType.put());
    if (FAILED(hr)) break;  // 枚举完了

    UINT32 numerator = 0, denominator = 0;
    hr = MFGetAttributeRatio(mediaType.get(), MF_MT_FRAME_RATE, &numerator,
                             &denominator);
    if (SUCCEEDED(hr)) {
      std::cout << "Input Type " << i << ": FrameRate = " << numerator << "/"
                << denominator << " = " << (double)numerator / denominator
                << " fps\n";
    } else {
      std::cout << "Input Type " << i << ": FrameRate not set\n";
    }
  }
}

void GpuEncoder::Cleanup() {
  transform_ = nullptr;
  deviceManager_ = nullptr;
  context_ = nullptr;
  device_ = nullptr;
  eventGen_ = nullptr;
}

winrt::com_ptr<ID3D11Texture2D> GpuEncoder::CreateBlackNV12Texture(int width,
                                                                   int height) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  // NV12：Y 平面 followed by UV 平面（交错）
  size_t ySize = width * height;
  size_t uvSize = ySize / 2;

  std::vector<uint8_t> blackData(ySize + uvSize, 0x00);  // 全 0，黑色帧
  D3D11_SUBRESOURCE_DATA initData = {};
  initData.pSysMem = blackData.data();
  initData.SysMemPitch = width;
  initData.SysMemSlicePitch = 0;

  winrt::com_ptr<ID3D11Texture2D> blackTexture;
  HRESULT hr = device_->CreateTexture2D(&desc, &initData, blackTexture.put());
  if (FAILED(hr)) {
    throw std::runtime_error("Failed to create black NV12 texture");
  }

  return blackTexture;
}

void GpuEncoder::Start() {
  if (running_) return;
  running_ = true;
  worker_ = std::thread([this] { EncodeLoop(); });
}

void GpuEncoder::Stop() {
  if (!running_) return;
  running_ = false;
  if (worker_.joinable()) worker_.join();
}

void GpuEncoder::EnqueueFrame(winrt::com_ptr<ID3D11Texture2D> frame) {
  D3D11_TEXTURE2D_DESC desc = {};
  frame->GetDesc(&desc);
  auto width = desc.Width;
  auto height = desc.Height;

  if (width != currentWidth || height != currentHeight) {
    currentWidth = width;
    currentHeight = height;

    ResetEncoder();
    return;
  }

  m_surfaceQueue.push(std::move(frame));
}

void GpuEncoder::EncodeLoop() {
  transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL);

  auto processInput = [&]() -> HRESULT {
    winrt::com_ptr<ID3D11Texture2D> surface;
    if (!m_surfaceQueue.pop(surface)) {
      surface = CreateBlackNV12Texture(currentWidth, currentHeight);
    }
    auto start_rgba_to_nv12 = std::chrono::high_resolution_clock::now();
    winrt::com_ptr<IMFMediaBuffer> inputBuffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), surface.get(), 0, FALSE, inputBuffer.put());
    if (FAILED(hr)) return hr;
    winrt::com_ptr<IMFSample> sample;
    hr = MFCreateSample(sample.put());
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(inputBuffer.get());
    if (FAILED(hr)) return hr;
    hr = transform_->ProcessInput(inputStreamID_, sample.get(), 0);
    if (FAILED(hr)) return hr;
    auto end_rgba_to_nv12 = std::chrono::high_resolution_clock::now();
    auto duration_rgba_to_nv12 =
        std::chrono::duration_cast<std::chrono::microseconds>(
            end_rgba_to_nv12 - start_rgba_to_nv12)
            .count();
    std::cout << "[编码NV12] Duration: " << std::dec << duration_rgba_to_nv12
              << "us\n";

    return S_OK;
  };

  while (running_) {
    // 等待事件
    winrt::com_ptr<IMFMediaEvent> ev;
    HRESULT hr = eventGen_->GetEvent(100, ev.put());

    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      // std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    MediaEventType type;
    hr = ev->GetType(&type);
    CHECK_HR(hr, "GetEventType failed");

    switch (type) {
      case METransformNeedInput: {
        CHECK(!(encoding.load()), "Expected METransformHaveOutput");
        encoding.store(true);

        HRESULT hr = processInput();
        break;
      }
      case METransformHaveOutput: {
        CHECK(encoding, "Expected METransformNeedInput");
        encoding.store(false);

        MFT_OUTPUT_DATA_BUFFER outputData = {};
        DWORD status = 0;

        winrt::com_ptr<IMFSample> outputSample;
        MFCreateSample(outputSample.put());

        winrt::com_ptr<IMFMediaBuffer> outBuffer;
        MFCreateMemoryBuffer(1024 * 1024, outBuffer.put());

        outputSample->AddBuffer(outBuffer.get());

        outputData.dwStreamID = outputStreamID_;
        hr = transform_->ProcessOutput(0, 1, &outputData, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        CHECK_HR(hr, "ProcessOutput failed");

        winrt::com_ptr<IMFSample> sample;
        if (outputData.pSample) {
          sample.attach(outputData.pSample);  // 接管指针

          // std::ofstream file("output.h264", std::ios::binary |
          // std::ios::app); WriteSampleToFile(sample.get(), file);
        }

        if (outputData.pEvents) outputData.pEvents->Release();

        break;
      }
      default:
        CHECK(true, "Unknown event");
        break;
    }
  }

  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, NULL);
  transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL);
}

void GpuEncoder::ResetEncoder() {
  // 1. 停止编码线程
  Stop();

  // 2. 释放旧编码器资源
  eventGen_ = nullptr;
  deviceManager_ = nullptr;
  attributes_ = nullptr;
  transform_ = nullptr;

  encoding.store(false);

  // 3. 重新初始化
  Init();

  // 4. 启动编码线程
  Start();
}

void GpuEncoder::WriteSampleToFile(IMFSample* sample, std::ofstream& file) {
  if (!sample || !file.is_open())
    throw std::runtime_error("Sample is null or file is not open");

  DWORD bufferCount = 0;
  HRESULT hr = sample->GetBufferCount(&bufferCount);
  if (FAILED(hr))
    throw std::runtime_error("Failed to get buffer count from sample");

  for (DWORD i = 0; i < bufferCount; ++i) {
    IMFMediaBuffer* buffer = nullptr;
    hr = sample->GetBufferByIndex(i, &buffer);
    if (FAILED(hr))
      throw std::runtime_error("Failed to get buffer from sample");

    BYTE* data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = buffer->Lock(&data, &maxLen, &curLen);
    if (FAILED(hr)) {
      buffer->Release();
      throw std::runtime_error("Failed to lock buffer");
    }

    // 写入 curLen 字节的数据到文件
    file.write(reinterpret_cast<const char*>(data), curLen);

    buffer->Unlock();
    buffer->Release();
  }
}
