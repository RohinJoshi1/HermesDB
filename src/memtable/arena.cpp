#include "hermesdb/arena.hpp"

namespace hermesdb {

Arena::Arena(std::size_t block_size)
    : block_size_(block_size < 4 * kAlign ? 4 * kAlign : block_size) {
  std::lock_guard lock(mutex_);
  current_.store(add_block(block_size_), std::memory_order_release);
}

Arena::Block* Arena::add_block(std::size_t bytes) {
  blocks_.push_back(std::make_unique<Block>(bytes));
  memory_usage_.fetch_add(bytes, std::memory_order_relaxed);
  return blocks_.back().get();
}

void* Arena::allocate(std::size_t bytes) {
  bytes = (bytes + kAlign - 1) & ~(kAlign - 1);
  if (bytes == 0) bytes = kAlign;
  // Large records get their own block so they do not strand the tail of the
  // shared one.
  if (bytes > block_size_ / 4) {
    std::lock_guard lock(mutex_);
    return add_block(bytes)->data.get();
  }
  for (;;) {
    Block* block = current_.load(std::memory_order_acquire);
    const auto offset = block->used.fetch_add(bytes, std::memory_order_relaxed);
    if (offset + bytes <= block->size) return block->data.get() + offset;
    std::lock_guard lock(mutex_);
    if (current_.load(std::memory_order_relaxed) == block) {
      current_.store(add_block(block_size_), std::memory_order_release);
    }
  }
}

}  // namespace hermesdb
