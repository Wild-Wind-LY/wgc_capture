#pragma once

#include <chrono>
#include <deque>
#include <string>

class FpsCounter {
public:
  explicit FpsCounter(double duration_secs = 1.0)
      : max_duration_(std::chrono::duration<double>(duration_secs)) {}

  // 在每帧开始或结束处调用
  void Tick() {
    auto now = std::chrono::steady_clock::now();
    frame_times_.push_back(now);

    // 清除过时帧
    while (!frame_times_.empty() && now - frame_times_.front() > max_duration_) {
      frame_times_.pop_front();
    }
  }

  double GetFPS() const {
    if (frame_times_.size() < 2) return 0.0;

    auto duration = std::chrono::duration<double>(frame_times_.back() - frame_times_.front());

    return static_cast<double>(frame_times_.size() - 1) / duration.count();
  }

  std::string GetFPSString() const {
    char buf[64];
    snprintf(buf, sizeof(buf), " %.2f", GetFPS());
    return std::string(buf);
  }

private:
  std::deque<std::chrono::steady_clock::time_point> frame_times_;
  std::chrono::duration<double> max_duration_;
};
