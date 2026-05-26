#pragma once

#ifdef BUILD_PYBIND11
#  include <pybind11/functional.h>
#  include <pybind11/pybind11.h>
#  include <pybind11/stl.h>

#  include "fps_counter.hpp"
#  include "monitor_selector.hpp"
#  include "wgc_core.hpp"
#  include "window_selector.hpp"

namespace py = pybind11;
using WindowInfo = WindowSelector::WindowInfo;
using MonitorInfo = MonitorSelector::MonitorInfo;

PYBIND11_MODULE(py_capture, m) {
  m.doc() = "Windows Graphics Capture binding using pybind11";

  py::class_<WindowInfo>(m, "WindowInfo")
      .def_readonly("hwnd", &WindowInfo::hwnd)
      .def_readonly("title", &WindowInfo::title)
      .def_readonly("class_name", &WindowInfo::className)
      .def("__repr__", [](const WindowInfo& info) {
        std::ostringstream oss;
        oss << "<WindowInfo hwnd=0x" << std::hex << info.hwnd << ", title=\"" << info.title
            << "\", class=\"" << info.className << "\">";
        return oss.str();
      });

  py::class_<MonitorInfo>(m, "MonitorInfo")
      .def_property_readonly(
          "hmonitor",
          [](const MonitorInfo& mi) { return reinterpret_cast<uintptr_t>(mi.hmonitor); })
      .def_readonly("device_name", &MonitorInfo::deviceName)
      .def_property_readonly("monitor_rect",
                             [](const MonitorInfo& mi) {
                               return py::make_tuple(mi.monitorRect.left, mi.monitorRect.top,
                                                     mi.monitorRect.right, mi.monitorRect.bottom);
                             })
      .def_property_readonly("work_rect",
                             [](const MonitorInfo& mi) {
                               return py::make_tuple(mi.workRect.left, mi.workRect.top,
                                                     mi.workRect.right, mi.workRect.bottom);
                             })
      .def_readonly("is_primary", &MonitorInfo::isPrimary);

  py::class_<EncodedFrame>(m, "EncodedFrame")
      .def(py::init<>())
      .def_readwrite("frame_index", &EncodedFrame::frameIndex)
      .def_property(
          "data",
          [](const EncodedFrame& self) {
            return py::bytes(reinterpret_cast<const char*>(self.data.data()), self.data.size());
          },
          [](EncodedFrame& self, py::bytes b) {
            std::string s = b;
            self.data.assign(s.begin(), s.end());
          })
      .def("__repr__", [](const EncodedFrame& f) {
        std::ostringstream oss;
        oss << "<EncodedFrame frame_index=" << f.frameIndex << ", data_size=" << f.data.size()
            << ">";
        return oss.str();
      });

  py::class_<FrameData>(m, "FrameData")
      .def(py::init<>())
      .def_readwrite("frame_index", &FrameData::frameIndex)
      .def_property_readonly("width", [](const FrameData& f) { return f.desc.Width; })
      .def_property_readonly("height", [](const FrameData& f) { return f.desc.Height; })
      .def_property(
          "rgba_data",
          [](const FrameData& self) {
            return py::bytes(reinterpret_cast<const char*>(self.rgbaData.data()),
                             self.rgbaData.size());
          },
          [](FrameData& self, py::bytes b) {
            std::string s = b;
            self.rgbaData.assign(s.begin(), s.end());
          })
      .def("__repr__", [](const FrameData& r) {
        std::ostringstream oss;
        oss << "<FrameData frame_index=" << r.frameIndex << ", width=" << r.desc.Width
            << ", height=" << r.desc.Height << ", rgba_data_size=" << r.rgbaData.size() << ">";
        return oss.str();
      });

  py::class_<WindowSelector>(m, "WindowSelector")
      .def(py::init<>())
      .def("add_title_blacklist", &WindowSelector::AddTitleBlacklist, py::arg("title"))
      .def("add_class_blacklist", &WindowSelector::AddClassBlacklist, py::arg("class_name"))
      .def("enumerate_windows", &WindowSelector::EnumerateWindows, "Refresh and list windows")
      .def("select_window_by_index", &WindowSelector::SelectWindowByIndex, py::arg("index"))
      .def("show_window_list", &WindowSelector::ShowWindowList);

  py::class_<MonitorSelector>(m, "MonitorSelector")
      .def(py::init<>())
      .def("enumerate_monitors", &MonitorSelector::EnumerateMonitors)
      .def("select_monitor_by_index", &MonitorSelector::SelectMonitorByIndex)
      .def("show_monitor_list", &MonitorSelector::ShowMonitorList);

  // 先定义类
  py::class_<WgcCore> cls(m, "WgcCore");

  // 枚举 CaptureType
  py::enum_<WgcCore::CaptureType>(cls, "CaptureType")
      .value("Window", WgcCore::CaptureType::Window)
      .value("Monitor", WgcCore::CaptureType::Monitor)
      .export_values();

  cls.def(py::init<>())
      .def("initialize", &WgcCore::Initialize, py::arg("hwnd_or_hmonitor"), py::arg("capture_type"),
           "Initialize capture for HWND or HMONITOR")
      .def("start", &WgcCore::Start, "Start capture")
      .def("stop", &WgcCore::Stop, "Stop capture")
      .def(
          "get_encoded_frame",
          [](WgcCore& self) -> py::object {
            auto opt = self.GetEncodedFrame();
            if (opt) return py::cast(*opt);
            return py::none();
          },
          "Get encoded frame (QOI format), returns None if not available")
      .def(
          "decode_qoi_to_frame",
          [](WgcCore& self, const EncodedFrame& frame) -> py::object {
            auto opt = self.DecodeQoiToFrame(frame);
            if (opt) return py::cast(*opt);
            return py::none();
          },
          "Decode QOI frame to raw RGBA data")
      .def_static("get_windows_version_string", &WgcCore::GetWindowsVersionString,
                  "Returns Windows version as a string");
}

#endif  // BUILD_PYBIND11
