#include "hermesdb/memtable.hpp"
#include "hermesdb/iterator.hpp"

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
    const int height = random_height();
    auto* node = new Node(key, std::move(value), height);

    for (;;) {
      if (find(key, preds, succs)) {
        Node* existing = succs[0];
        std::lock_guard lock(existing->mutex);
        const auto old = existing->value.size();
        existing->value = std::move(node->value);
        delete node;
        if (existing->value.size() >= old) {
          approximate_size_.fetch_add(existing->value.size() - old,
                                      std::memory_order_relaxed);
        } else {
          approximate_size_.fetch_sub(old - existing->value.size(),
                                      std::memory_order_relaxed);
        }
        return;
      }

      node->next[0].store(succs[0], std::memory_order_relaxed);
      Node* expected = succs[0];
      if (preds[0]->next[0].compare_exchange_strong(
              expected, node, std::memory_order_release,
              std::memory_order_acquire)) {
        break;
      }
    }

    approximate_size_.fetch_add(
        node->key.user_key().size() + 8 + node->value.size(),
        std::memory_order_relaxed);

    int top = max_height_.load(std::memory_order_relaxed);
    while (height > top && !max_height_.compare_exchange_weak(
                               top, height, std::memory_order_relaxed)) {
    }

    // Level 0 owns the node. Higher levels are best-effort index links:
    // readers can always descend to level 0 while these CAS loops complete.
    for (int level = 1; level < height; ++level) {
      const auto index = static_cast<std::size_t>(level);
      for (;;) {
        static_cast<void>(find(key, preds, succs));
        node->next[index].store(succs[index], std::memory_order_relaxed);
        Node* expected = succs[index];
        if (preds[index]->next[index].compare_exchange_strong(
                expected, node, std::memory_order_release,
                std::memory_order_acquire)) {
          break;
        }
      }
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

std::size_t MemTable::approximate_size() const {
  return list_->approximate_size_.load(std::memory_order_relaxed);
}

bool MemTable::empty() const {
  return list_->load(list_->head_.next[0]) == nullptr;
}

class MemTable::Cursor final : public StorageIterator {
 public:
  Cursor(std::shared_ptr<MemTable> owner, SkipList* list,
         const InternalKey& lower, std::optional<InternalKey> upper)
      : owner_(std::move(owner)), list_(list), upper_(std::move(upper)) {
    node_ = list_->lower_bound(lower);
    load();
  }

  [[nodiscard]] bool valid() const noexcept override { return valid_; }

  [[nodiscard]] const InternalKey& key() const override {
    if (!valid_ || node_ == nullptr) throw Error("iterator is invalid");
    return node_->key;
  }

  [[nodiscard]] InternalKeyView key_view() const override {
    if (!valid_ || node_ == nullptr) throw Error("iterator is invalid");
    return as_view(node_->key);
  }

  [[nodiscard]] ByteView value() const override {
    if (!valid_) throw Error("iterator is invalid");
    return value_;
  }

  void next() override {
    if (node_ == nullptr) return;
    node_ = list_->load(node_->next[0]);
    load();
  }

 private:
  void load() {
    valid_ = false;
    value_.clear();
    if (node_ == nullptr) return;
    if (upper_.has_value() &&
        compare_internal(as_view(node_->key), as_view(*upper_)) >= 0) {
      node_ = nullptr;
      return;
    }
    std::lock_guard lock(node_->mutex);
    value_ = node_->value;
    valid_ = true;
  }

  std::shared_ptr<MemTable> owner_;
  SkipList* list_{};
  SkipList::Node* node_{};
  std::optional<InternalKey> upper_;
  Bytes value_;
  bool valid_{false};
};

IteratorPtr MemTable::iter() const { return iter_from(InternalKey{}); }

IteratorPtr MemTable::scan(const InternalKey& lower,
                           const InternalKey& upper) const {
  return iter_from(lower, upper);
}

IteratorPtr MemTable::iter_from(const InternalKey& lower,
                                std::optional<InternalKey> upper) const {
  return std::make_unique<Cursor>(nullptr, list_.get(), lower, upper);
}

IteratorPtr MemTable::iter_from(std::shared_ptr<MemTable> table,
                                const InternalKey& lower,
                                std::optional<InternalKey> upper) {
  auto* list = table->list_.get();
  return std::make_unique<Cursor>(std::move(table), list, lower, upper);
}

}  // namespace hermesdb
