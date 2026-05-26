#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

template <typename T, size_t Capacity> class LockFreeFrameQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
  LockFreeFrameQueue() = default;

  // 禁止拷贝
  LockFreeFrameQueue(const LockFreeFrameQueue&) = delete;
  LockFreeFrameQueue& operator=(const LockFreeFrameQueue&) = delete;

  // 生产者写入（成功返回 true，失败返回 false）
  bool push(T&& item) {
    size_t currentWrite = m_writeIndex.load(std::memory_order_relaxed);
    size_t nextWrite = (currentWrite + 1) & kMask;

    if (nextWrite == m_readIndex.load(std::memory_order_acquire)) {
      return false;  // Full
    }

    m_buffer[currentWrite].emplace(std::move(item));
    m_writeIndex.store(nextWrite, std::memory_order_release);
    m_cv.notify_one();
    return true;
  }

  // 消费者阻塞读取（超时返回 false）
  bool pop(T& outItem, uint32_t timeoutMs = 100) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
          return m_readIndex.load(std::memory_order_acquire)
                 != m_writeIndex.load(std::memory_order_acquire);
        })) {
      return false;  // Timeout
    }

    size_t currentRead = m_readIndex.load(std::memory_order_relaxed);
    auto& opt = m_buffer[currentRead];
    if (opt.has_value()) {
      outItem = std::move(opt.value());
      opt.reset();
      m_readIndex.store((currentRead + 1) & kMask, std::memory_order_release);
      return true;
    }
    return false;
  }

  // 非阻塞读取（成功返回 true，失败返回 false）
  bool try_pop(T& outItem) {
    size_t currentRead = m_readIndex.load(std::memory_order_relaxed);
    if (currentRead == m_writeIndex.load(std::memory_order_acquire)) {
      return false;  // Empty
    }

    auto& opt = m_buffer[currentRead];
    if (opt.has_value()) {
      outItem = std::move(opt.value());
      opt.reset();
      m_readIndex.store((currentRead + 1) & kMask, std::memory_order_release);
      return true;
    }
    return false;
  }

  bool empty() const { return m_readIndex.load() == m_writeIndex.load(); }

  bool full() const { return ((m_writeIndex.load() + 1) & kMask) == m_readIndex.load(); }

private:
  static constexpr size_t kMask = Capacity - 1;
  std::array<std::optional<T>, Capacity> m_buffer;
  std::atomic<size_t> m_writeIndex = 0;
  std::atomic<size_t> m_readIndex = 0;

  std::condition_variable m_cv;
  mutable std::mutex m_mutex;  // only for condition_variable wait
};
