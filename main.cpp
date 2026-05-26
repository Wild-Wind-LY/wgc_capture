/**
 * @file main.cpp
 * @author liuyan (liuyan@qq.com)
 * @brief
 * @version 0.1
 * @date 2025-07-19
 *
 * @copyright Copyright (c) 2025
 *
 */

#include <iostream>

#include "fps_counter.hpp"
#include "frame_renderer.hpp"
#include "monitor_selector.hpp"
#include "rubber_band_box.hpp"
#include "wgc_core.hpp"
#include "window_selector.hpp"

void ConvertRGBAtoBGRA(uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; i += 4) {
    std::swap(data[i], data[i + 2]);  // R <-> B
  }
}

int main() {
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  std::wcout.imbue(std::locale(""));

  auto region = RubberBandBox::SelectRegion(GetModuleHandle(nullptr));
  if (!region) {
    std::cout << "未选中任何区域\n";
  } else {
    auto r = *region;
    std::cout << "选中区域: " << r.left << "," << r.top << " - " << r.right << "," << r.bottom
              << "\n";
  }

#if 0  // 测试屏幕捕获
  MonitorSelector selector;
  auto monitors = selector.EnumerateMonitors();
  selector.ShowMonitorList();

  int index;
  std::cout << "请输入要选择的显示器索引: ";
  std::cin >> index;

  auto selected = selector.SelectMonitorByIndex(index);
  if (selected) {
    std::cout << "已选择显示器句柄: " << selected << std::endl;
  } else {
    std::cout << "无效的选择。" << std::endl;
    return 0;
  }

#else  // 测试窗口捕获
  WindowSelector selector;
  selector.AddClassBlacklist("Shell_TrayWnd");    // 任务栏
  selector.AddTitleBlacklist("Program Manager");  // 桌面
  //   selector.AddClassBlacklist("Progman");                 // 桌面管理器
  selector.AddClassBlacklist("WorkerW");                 // 桌面容器
  selector.AddClassBlacklist("IME");                     // 输入法
  selector.AddClassBlacklist("EdgeUiInputTopWndClass");  // 输入体验

  auto windows = selector.EnumerateWindows();
  selector.ShowWindowList();

  int choice;
  std::cout << "输入要捕捉的窗口编号: ";
  std::cin >> choice;

  auto selected = selector.SelectWindowByIndex(choice);
  if (selected) {
    std::cout << "选择的窗口句柄: 0x" << std::hex << selected << std::endl;
  } else {
    std::cout << "无效选择" << std::endl;
    return 0;
  }
#endif

  try {
    WgcCore capturer;
    FpsCounter fps;
    if (!capturer.Initialize(selected, WgcCore::CaptureType::Window)) {
      std::cout << "初始化捕获失败" << std::endl;
      return -1;
    }

    capturer.Start();

    FrameRenderer renderer("WGC Preview", 120);

    while (renderer.Run()) {
      auto bufOpt = capturer.GetEncodedFrame();
      if (!bufOpt) continue;
      auto& buffer = *bufOpt;

      ConvertRGBAtoBGRA(buffer.data.data(), buffer.data.size());
      renderer.SubmitFrame({0, buffer.desc, buffer.data});

      // auto rawFrameOpt = capturer.DecodeQoiToFrame(buffer);
      // if (!rawFrameOpt) continue;
      // FrameData framedata = *rawFrameOpt;
      // ConvertRGBAtoBGRA(framedata.rgbaData.data(),
      // framedata.rgbaData.size()); renderer.SubmitFrame(framedata);

#if 0
        // cv::cvtColor(frame, frame, cv::COLOR_RGBA2BGRA);
        // cv::Mat frame;
        // if (!buffer.empty()) {
        //   frame = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
        // }
        fps.Tick();
        if (!frame.empty()) {
          cv::putText(frame, fps.GetFPSString(), cv::Point(10, 35),
                      cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(0, 0, 255),
                      2);
        }

        if (!frame.empty()) {
          cv::imshow("WGC Preview", frame);
        }

#endif
    }

    capturer.Stop();
  } catch (const std::exception& e) {
    std::cerr << "捕获异常: " << e.what() << std::endl;
  }

  return 0;
}
