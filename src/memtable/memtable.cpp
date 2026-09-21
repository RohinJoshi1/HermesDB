#include "hermesdb/memtable.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

namespace hermesdb {
namespace {

constexpr int kMaxHeight = 16;

int random_height() {
  thread_local std::mt19937 rng{std::random_device{}()};
  int height = 1;
  while (height < kMaxHeight && (rng() & 0x3U) == 0) ++height;
  return height;
}

}  // namespace

struct MemTable::SkipList {
  struct Node {
    Node(InternalKey key_arg, Bytes value_arg, int height_arg)
        : key(std::move(key_arg)),
          value(std::move(value_arg)),
          height(height_arg) {
      for (auto& link : next) link.store(nullptr, std::memory_order_relaxed);
    }

    InternalKey key;
    Bytes value;
    const int height;
    mutable std::mutex mutex;
    std::array<std::atomic<Node*>, kMaxHeight> next{};
  };

  SkipList() : head_(InternalKey(), Bytes{}, kMaxHeight) {}

  ~SkipList() {
    Node* node = head_.next[0].load(std::memory_order_relaxed);
    while (node != nullptr) {
      Node* successor = node->next[0].load(std::memory_order_relaxed);
      delete node;
      node = successor;
    }
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  [[nodiscard]] Node* load(const std::atomic<Node*>& link) const {
    return link.load(std::memory_order_acquire);
  }

  bool find(const InternalKey& key, std::array<Node*, kMaxHeight>& preds,
            std::array<Node*, kMaxHeight>& succs) const {
    Node* pred = const_cast<Node*>(&head_);
    for (int level = kMaxHeight - 1; level >= 0; --level) {
      Node* current = load(pred->next[static_cast<std::size_t>(level)]);
      while (current != nullptr && current->key < key) {
        pred = current;
        current = load(pred->next[static_cast<std::size_t>(level)]);
      }
      preds[static_cast<std::size_t>(level)] = pred;
      succs[static_cast<std::size_t>(level)] = current;
    }
    Node* match = succs[0];
    return match != nullptr && match->key == key;
  }

  void insert(const InternalKey& key, Bytes value) {
    std::array<Node*, kMaxHeight> preds{};
    std::array<Node*, kMaxHeight> succs{};
    for (;;) {
      if (find(key, preds, succs)) {
        Node* existing = succs[0];
        std::lock_guard lock(existing->mutex);
        if (existing->key == key) {
          const auto old = existing->value.size();
          existing->value = std::move(value);
          if (existing->value.size() >= old) {
            approximate_size_.fetch_add(existing->value.size() - old,
                                        std::memory_order_relaxed);
          } else {
            approximate_size_.fetch_sub(old - existing->value.size(),
                                        std::memory_order_relaxed);
          }
          return;
        }
        continue;
      }

      const int height = random_height();
      int top = max_height_.load(std::memory_order_relaxed);
      while (height > top && !max_height_.compare_exchange_weak(
                                 top, height, std::memory_order_relaxed)) {
      }

      std::vector<Node*> lock_order;
      lock_order.reserve(static_cast<std::size_t>(height));
      for (int level = 0; level < height; ++level) {
        lock_order.push_back(preds[static_cast<std::size_t>(level)]);
      }
      std::sort(lock_order.begin(), lock_order.end());
      lock_order.erase(std::unique(lock_order.begin(), lock_order.end()),
                       lock_order.end());

      std::vector<std::unique_lock<std::mutex>> guards;
      guards.reserve(lock_order.size());
      for (Node* node : lock_order) {
        guards.emplace_back(node->mutex);
      }

      bool stale = false;
      for (int level = 0; level < height; ++level) {
        if (load(preds[static_cast<std::size_t>(level)]
                     ->next[static_cast<std::size_t>(level)]) !=
            succs[static_cast<std::size_t>(level)]) {
          stale = true;
          break;
        }
      }
      if (stale) continue;

      auto* node = new Node(key, std::move(value), height);
      approximate_size_.fetch_add(
          node->key.user_key().size() + 8 + node->value.size(),
          std::memory_order_relaxed);
      for (int level = 0; level < height; ++level) {
        node->next[static_cast<std::size_t>(level)].store(
            succs[static_cast<std::size_t>(level)], std::memory_order_relaxed);
        preds[static_cast<std::size_t>(level)]
            ->next[static_cast<std::size_t>(level)]
            .store(node, std::memory_order_release);
      }
      return;
    }
  }

  [[nodiscard]] Node* lower_bound(const InternalKey& key) const {
    Node* pred = const_cast<Node*>(&head_);
    for (int level = kMaxHeight - 1; level >= 0; --level) {
      Node* current = load(pred->next[static_cast<std::size_t>(level)]);
      while (current != nullptr && current->key < key) {
        pred = current;
        current = load(pred->next[static_cast<std::size_t>(level)]);
      }
    }
    return load(pred->next[0]);
  }

  Node head_;
  std::atomic<int> max_height_{1};
  std::atomic<std::size_t> approximate_size_{0};
};

MemTable::MemTable() : list_(std::make_unique<SkipList>()) {}
MemTable::~MemTable() = default;

void MemTable::put(ByteView key, ByteView value) { put(key, 0, value); }

void MemTable::put(ByteView key, std::uint64_t timestamp, ByteView value) {
  list_->insert(InternalKey(Bytes(key.begin(), key.end()), timestamp),
                Bytes(value.begin(), value.end()));
}

void MemTable::put(std::string_view key, std::string_view value) {
  put(as_bytes(key), as_bytes(value));
}

void MemTable::put(std::string_view key, std::uint64_t timestamp,
                   ByteView value) {
  put(as_bytes(key), timestamp, value);
}

void MemTable::erase(ByteView key, std::uint64_t timestamp) {
  put(key, timestamp, ByteView{});
}

void MemTable::erase(std::string_view key, std::uint64_t timestamp) {
  erase(as_bytes(key), timestamp);
}

void MemTable::put_batch(std::span<const std::pair<Bytes, Bytes>> entries) {
  for (const auto& [key, value] : entries) {
    list_->insert(InternalKey(key), value);
  }
}

void MemTable::put_batch(std::span<const KeyValue> entries) {
  for (const auto& [key, value] : entries) list_->insert(key, value);
}

std::optional<Bytes> MemTable::get(ByteView key) const {
  return get(key, kMaxTimestamp);
}

std::optional<Bytes> MemTable::get(ByteView key,
                                   std::uint64_t read_timestamp) const {
  const InternalKey target(Bytes(key.begin(), key.end()), read_timestamp);
  SkipList::Node* node = list_->lower_bound(target);
  if (node == nullptr ||
      !std::equal(node->key.user_key().begin(), node->key.user_key().end(),
                  key.begin(), key.end())) {
    return std::nullopt;
  }
  std::lock_guard lock(node->mutex);
  return node->value;
}

std::optional<Bytes> MemTable::get(std::string_view key) const {
  return get(as_bytes(key));
}

std::optional<Bytes> MemTable::get(std::string_view key,
                                   std::uint64_t read_timestamp) const {
  return get(as_bytes(key), read_timestamp);
}

std::vector<KeyValue> MemTable::entries() const {
  std::vector<KeyValue> all;
  for (auto* node = list_->load(list_->head_.next[0]); node != nullptr;
       node = list_->load(node->next[0])) {
    std::lock_guard lock(node->mutex);
    all.emplace_back(node->key, node->value);
  }
  return all;
}

IteratorPtr MemTable::iter() const {
  return std::make_unique<VectorIterator>(entries());
}

IteratorPtr MemTable::scan(const InternalKey& lower,
                           const InternalKey& upper) const {
  std::vector<KeyValue> result;
  for (auto* node = list_->lower_bound(lower);
       node != nullptr && node->key < upper;
       node = list_->load(node->next[0])) {
    std::lock_guard lock(node->mutex);
    result.emplace_back(node->key, node->value);
  }
  return std::make_unique<VectorIterator>(std::move(result));
}

std::size_t MemTable::approximate_size() const {
  return list_->approximate_size_.load(std::memory_order_relaxed);
}

bool MemTable::empty() const {
  return list_->load(list_->head_.next[0]) == nullptr;
}

}  // namespace hermesdb
