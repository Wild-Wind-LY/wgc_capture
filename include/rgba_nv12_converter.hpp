#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <winrt/base.h>

#include <stdexcept>

class RgbaNv12ConverterD3D11 {
public:
  RgbaNv12ConverterD3D11(winrt::com_ptr<ID3D11Device> device) : device_(device) {
    if (!device_) throw std::invalid_argument("device must not be null");
    device_->GetImmediateContext(context_.put());

    if (!videoDevice_) device_.as(videoDevice_);
    if (!videoContext_) context_.as(videoContext_);
  }

  void ConvertRGBAtoNV12(winrt::com_ptr<ID3D11Texture2D> srcRGBA,
                         winrt::com_ptr<ID3D11Texture2D>& outNV12) {
    if (!srcRGBA) throw std::invalid_argument("srcRGBA is null");

    D3D11_TEXTURE2D_DESC desc = {};
    srcRGBA->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
      throw std::runtime_error("Invalid texture size.");
    }

    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
      throw std::runtime_error("Only RGBA input supported");

    bool needsEnumeratorUpdate
        = (widthRGBAtoNV12_ != desc.Width) || (heightRGBAtoNV12_ != desc.Height);

    if (needsEnumeratorUpdate) {
      widthRGBAtoNV12_ = desc.Width;
      heightRGBAtoNV12_ = desc.Height;

      D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
      contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
      contentDesc.InputWidth = widthRGBAtoNV12_;
      contentDesc.InputHeight = heightRGBAtoNV12_;
      contentDesc.OutputWidth = widthRGBAtoNV12_;
      contentDesc.OutputHeight = heightRGBAtoNV12_;
      contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

      winrt::check_hresult(
          videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, enumeratorRGBAtoNV12_.put()));

      auto CheckFormatSupport = [this](DXGI_FORMAT format, const char* label) {
        UINT supported = FALSE;
        winrt::check_hresult(enumeratorRGBAtoNV12_->CheckVideoProcessorFormat(format, &supported));
        if (!supported) {
          throw std::runtime_error(std::string(label) + " not supported.");
        }
      };

      // 检查输入输出格式
      CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, "Input format DXGI_FORMAT_R8G8B8A8_UNORM");
      CheckFormatSupport(DXGI_FORMAT_NV12, "Output format DXGI_FORMAT_NV12");

      winrt::check_hresult(videoDevice_->CreateVideoProcessor(enumeratorRGBAtoNV12_.get(), 0,
                                                              processorRGBAtoNV12_.put()));
    }

    bool needNewOutput = !outNV12;
    if (outNV12) {
      D3D11_TEXTURE2D_DESC outDesc = {};
      outNV12->GetDesc(&outDesc);
      if (outDesc.Width != desc.Width || outDesc.Height != desc.Height) needNewOutput = true;
    }
    // Create NV12 output texture
    if (needNewOutput) {
      D3D11_TEXTURE2D_DESC nv12Desc = {};
      nv12Desc.Width = (desc.Width);
      nv12Desc.Height = (desc.Height);
      nv12Desc.MipLevels = 1;
      nv12Desc.ArraySize = 1;
      nv12Desc.Format = DXGI_FORMAT_NV12;
      nv12Desc.SampleDesc.Count = 1;
      nv12Desc.Usage = D3D11_USAGE_DEFAULT;
      nv12Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

      winrt::check_hresult(device_->CreateTexture2D(&nv12Desc, nullptr, outNV12.put()));

      assert(nv12Desc.Format == DXGI_FORMAT_NV12);
      assert(nv12Desc.BindFlags & D3D11_BIND_RENDER_TARGET);
      assert((nv12Desc.Width % 2) == 0 && (nv12Desc.Height % 2) == 0);  // NV12 要求
    }

    // Create views
    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> outputView;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = 0;

    winrt::check_hresult(videoDevice_->CreateVideoProcessorInputView(
        srcRGBA.get(), enumeratorRGBAtoNV12_.get(), &inputDesc, inputView.put()));

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;

    winrt::check_hresult(videoDevice_->CreateVideoProcessorOutputView(
        outNV12.get(), enumeratorRGBAtoNV12_.get(), &outputDesc, outputView.put()));

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.get();
    stream.OutputIndex = 0;

    winrt::com_ptr<ID3D11VideoContext> videoContext;
    context_.as(videoContext);

    winrt::check_hresult(videoContext->VideoProcessorBlt(processorRGBAtoNV12_.get(),
                                                         outputView.get(), 0, 1, &stream));
  }

  void ConvertNV12ToRGBA(winrt::com_ptr<ID3D11Texture2D> srcNV12,
                         winrt::com_ptr<ID3D11Texture2D>& outRGBA) {
    if (!srcNV12) throw std::invalid_argument("srcNV12 is null");

    D3D11_TEXTURE2D_DESC desc = {};
    srcNV12->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) throw std::runtime_error("Only NV12 input supported");

    if (desc.Width != widthNV12toRGBA_ || desc.Height != heightNV12toRGBA_) {
      widthNV12toRGBA_ = desc.Width;
      heightNV12toRGBA_ = desc.Height;

      // 重新创建 VideoProcessorEnumerator 和 VideoProcessor
      D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
      contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
      contentDesc.InputWidth = widthNV12toRGBA_;
      contentDesc.InputHeight = heightNV12toRGBA_;
      contentDesc.OutputWidth = widthNV12toRGBA_;
      contentDesc.OutputHeight = heightNV12toRGBA_;
      contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

      winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator;
      winrt::check_hresult(
          videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, enumerator.put()));
      enumeratorNV12toRGBA_ = enumerator;

      winrt::com_ptr<ID3D11VideoProcessor> processor;
      winrt::check_hresult(
          videoDevice_->CreateVideoProcessor(enumeratorNV12toRGBA_.get(), 0, processor.put()));
      processorNV12toRGBA_ = processor;
    }

    bool needNewOutput = !outRGBA;
    if (outRGBA) {
      D3D11_TEXTURE2D_DESC outDesc = {};
      outRGBA->GetDesc(&outDesc);
      if (outDesc.Width != desc.Width || outDesc.Height != desc.Height) needNewOutput = true;
    }
    if (needNewOutput) {
      D3D11_TEXTURE2D_DESC rgbaDesc = {};
      rgbaDesc.Width = desc.Width;
      rgbaDesc.Height = desc.Height;
      rgbaDesc.MipLevels = 1;
      rgbaDesc.ArraySize = 1;
      rgbaDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rgbaDesc.SampleDesc.Count = 1;
      rgbaDesc.Usage = D3D11_USAGE_DEFAULT;
      rgbaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

      winrt::check_hresult(device_->CreateTexture2D(&rgbaDesc, nullptr, outRGBA.put()));
    }

    // 创建视图
    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = 0;
    winrt::check_hresult(videoDevice_->CreateVideoProcessorInputView(
        srcNV12.get(), enumeratorNV12toRGBA_.get(), &inputDesc, inputView.put()));

    winrt::com_ptr<ID3D11VideoProcessorOutputView> outputView;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    winrt::check_hresult(videoDevice_->CreateVideoProcessorOutputView(
        outRGBA.get(), enumeratorNV12toRGBA_.get(), &outputDesc, outputView.put()));

    // Video Processor BLT
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.get();
    stream.OutputIndex = 0;

    winrt::com_ptr<ID3D11VideoContext> videoContext;
    context_.as(videoContext);

    winrt::check_hresult(videoContext->VideoProcessorBlt(processorNV12toRGBA_.get(),
                                                         outputView.get(), 0, 1, &stream));
  }

private:
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<ID3D11VideoDevice> videoDevice_;
  winrt::com_ptr<ID3D11VideoContext> videoContext_;

  winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumeratorRGBAtoNV12_;
  winrt::com_ptr<ID3D11VideoProcessor> processorRGBAtoNV12_;

  winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumeratorNV12toRGBA_;
  winrt::com_ptr<ID3D11VideoProcessor> processorNV12toRGBA_;

  UINT widthRGBAtoNV12_ = 0;
  UINT heightRGBAtoNV12_ = 0;
  UINT widthNV12toRGBA_ = 0;
  UINT heightNV12toRGBA_ = 0;
};
