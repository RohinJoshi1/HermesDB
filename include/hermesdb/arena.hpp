#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace hermesdb {

// Concurrent bump allocator. Memory is released only when the arena is
// destroyed; objects placed in it must be trivially destructible.
class Arena {
 public:
  static constexpr std::size_t kAlign = alignof(std::max_align_t) < 8
                                            ? 8
                                            : alignof(std::max_align_t);
  static constexpr std::size_t kDefaultBlockSize = 64U << 10U;

  explicit Arena(std::size_t block_size = kDefaultBlockSize);
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns kAlign-aligned storage. Safe to call from many threads.
  [[nodiscard]] void* allocate(std::size_t bytes);
  [[nodiscard]] std::size_t memory_usage() const noexcept {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  struct Block {
    explicit Block(std::size_t bytes)
        : data(new std::byte[bytes]), size(bytes) {}
    std::unique_ptr<std::byte[]> data;
    std::size_t size;
    std::atomic<std::size_t> used{0};
  };

  Block* add_block(std::size_t bytes);

  const std::size_t block_size_;
  std::atomic<Block*> current_{nullptr};
  std::mutex mutex_;
  std::vector<std::unique_ptr<Block>> blocks_;
  std::atomic<std::size_t> memory_usage_{0};
};

}  // namespace hermesdb
