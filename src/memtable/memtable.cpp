#include "hermesdb/memtable.hpp"
#include "hermesdb/arena.hpp"
#include "hermesdb/iterator.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <type_traits>
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

// Nodes and values live in the memtable's arena and are never freed
// individually. A value is immutable once published; an overwrite of the
// same (key, timestamp) publishes a new record, so readers need no lock.
struct MemTable::SkipList {
  struct ValueRecord {
    std::uint64_t size;
    [[nodiscard]] ByteView view() const noexcept {
      return {reinterpret_cast<const Byte*>(this + 1),
              static_cast<std::size_t>(size)};
    }
  };

  // Layout: Node | std::atomic<Node*>[height] | user key bytes.
  struct Node {
    std::atomic<const ValueRecord*> value;
    std::uint64_t timestamp;
    std::uint32_t key_size;
    std::uint32_t height;

    [[nodiscard]] std::atomic<Node*>* tower() noexcept {
      return std::launder(reinterpret_cast<std::atomic<Node*>*>(
          reinterpret_cast<std::byte*>(this) + sizeof(Node)));
    }
    [[nodiscard]] const std::atomic<Node*>* tower() const noexcept {
      return const_cast<Node*>(this)->tower();
    }
    [[nodiscard]] std::atomic<Node*>& next(int level) noexcept {
      return tower()[level];
    }
    [[nodiscard]] ByteView user_key() const noexcept {
      return {reinterpret_cast<const Byte*>(tower() + height), key_size};
    }
    [[nodiscard]] InternalKeyView view() const noexcept {
      return {user_key(), timestamp};
    }
    [[nodiscard]] ByteView value_view() const noexcept {
      return value.load(std::memory_order_acquire)->view();
    }
  };
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0);
  static_assert(std::is_trivially_destructible_v<std::atomic<Node*>>);

  SkipList() : head_(make_node({}, &kEmptyValue, kMaxHeight)) {}

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  [[nodiscard]] static Node* load(const std::atomic<Node*>& link) {
    return link.load(std::memory_order_acquire);
  }

  [[nodiscard]] Node* make_node(InternalKeyView key, const ValueRecord* value,
                                int height) {
    const auto user = key.prefix();
    const std::size_t tower_bytes =
        static_cast<std::size_t>(height) * sizeof(std::atomic<Node*>);
    void* memory = arena_.allocate(sizeof(Node) + tower_bytes + user.size());
    auto* node = new (memory) Node{};
    node->value.store(value, std::memory_order_relaxed);
    node->timestamp = key.timestamp();
    node->key_size = static_cast<std::uint32_t>(user.size());
    node->height = static_cast<std::uint32_t>(height);
    auto* links = reinterpret_cast<std::byte*>(node) + sizeof(Node);
    for (int level = 0; level < height; ++level) {
      new (links + static_cast<std::size_t>(level) * sizeof(std::atomic<Node*>))
          std::atomic<Node*>(nullptr);
    }
    if (!user.empty()) {
      std::memcpy(links + tower_bytes, user.data(), user.size());
    }
    return node;
  }

  [[nodiscard]] const ValueRecord* make_value(ByteView value) {
    if (value.empty()) return &kEmptyValue;
    void* memory = arena_.allocate(sizeof(ValueRecord) + value.size());
    auto* record = new (memory) ValueRecord{value.size()};
    std::memcpy(record + 1, value.data(), value.size());
    return record;
  }

  bool find(const InternalKeyView& key, std::array<Node*, kMaxHeight>& preds,
            std::array<Node*, kMaxHeight>& succs) const {
    Node* pred = head_;
    for (int level = kMaxHeight - 1; level >= 0; --level) {
      Node* current = load(pred->next(level));
      while (current != nullptr && compare_internal(current->view(), key) < 0) {
        pred = current;
        current = load(pred->next(level));
      }
      preds[static_cast<std::size_t>(level)] = pred;
      succs[static_cast<std::size_t>(level)] = current;
    }
    Node* match = succs[0];
    return match != nullptr && compare_internal(match->view(), key) == 0;
  }

  void insert(ByteView user_key, std::uint64_t timestamp, ByteView value) {
    const InternalKeyView key(user_key, timestamp);
    const ValueRecord* record = make_value(value);
    std::array<Node*, kMaxHeight> preds{};
    std::array<Node*, kMaxHeight> succs{};
    const int height = random_height();
    Node* node = nullptr;

    for (;;) {
      if (find(key, preds, succs)) {
        const auto* old = succs[0]->value.exchange(record,
                                                   std::memory_order_acq_rel);
        if (record->size >= old->size) {
          approximate_size_.fetch_add(record->size - old->size,
                                      std::memory_order_relaxed);
        } else {
          approximate_size_.fetch_sub(old->size - record->size,
                                      std::memory_order_relaxed);
        }
        return;
      }
      if (node == nullptr) node = make_node(key, record, height);
      node->next(0).store(succs[0], std::memory_order_relaxed);
      Node* expected = succs[0];
      if (preds[0]->next(0).compare_exchange_strong(
              expected, node, std::memory_order_release,
              std::memory_order_acquire)) {
        break;
      }
    }

    approximate_size_.fetch_add(user_key.size() + 8 + value.size(),
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
        node->next(level).store(succs[index], std::memory_order_relaxed);
        Node* expected = succs[index];
        if (preds[index]->next(level).compare_exchange_strong(
                expected, node, std::memory_order_release,
                std::memory_order_acquire)) {
          break;
        }
      }
    }
  }

  [[nodiscard]] Node* lower_bound(const InternalKeyView& key) const {
    Node* pred = head_;
    for (int level = kMaxHeight - 1; level >= 0; --level) {
      Node* current = load(pred->next(level));
      while (current != nullptr && compare_internal(current->view(), key) < 0) {
        pred = current;
        current = load(pred->next(level));
      }
    }
    return load(pred->next(0));
  }

  [[nodiscard]] Node* first() const { return load(head_->next(0)); }

  static constexpr ValueRecord kEmptyValue{0};

  Arena arena_;
  Node* head_;
  std::atomic<int> max_height_{1};
  std::atomic<std::size_t> approximate_size_{0};
};

MemTable::MemTable() : list_(std::make_unique<SkipList>()) {}
MemTable::~MemTable() = default;

void MemTable::put(ByteView key, ByteView value) { put(key, 0, value); }

void MemTable::put(ByteView key, std::uint64_t timestamp, ByteView value) {
  list_->insert(key, timestamp, value);
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

void MemTable::put_batch(std::span<const std::pair<Bytes, Bytes>> entries,
                         std::uint64_t timestamp) {
  for (const auto& [key, value] : entries) {
    list_->insert(key, timestamp, value);
  }
}

void MemTable::put_batch(std::span<const KeyValue> entries) {
  for (const auto& [key, value] : entries) {
    list_->insert(key.user_key(), key.timestamp(), value);
  }
}

std::optional<Bytes> MemTable::get(ByteView key) const {
  return get(key, kMaxTimestamp);
}

std::optional<Bytes> MemTable::get(ByteView key,
                                   std::uint64_t read_timestamp) const {
  SkipList::Node* node =
      list_->lower_bound(InternalKeyView(key, read_timestamp));
  if (node == nullptr || compare_bytes(node->user_key(), key) != 0) {
    return std::nullopt;
  }
  const auto value = node->value_view();
  return Bytes(value.begin(), value.end());
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
  for (auto* node = list_->first(); node != nullptr;
       node = SkipList::load(node->next(0))) {
    const auto user = node->user_key();
    const auto value = node->value_view();
    all.emplace_back(InternalKey(Bytes(user.begin(), user.end()),
                                 node->timestamp),
                     Bytes(value.begin(), value.end()));
  }
  return all;
}

std::size_t MemTable::approximate_size() const {
  return list_->approximate_size_.load(std::memory_order_relaxed);
}

std::size_t MemTable::memory_usage() const {
  return list_->arena_.memory_usage();
}

bool MemTable::empty() const { return list_->first() == nullptr; }

class MemTable::Cursor final : public StorageIterator {
 public:
  Cursor(std::shared_ptr<MemTable> owner, SkipList* list,
         const InternalKey& lower, std::optional<InternalKey> upper)
      : owner_(std::move(owner)), list_(list), upper_(std::move(upper)) {
    node_ = list_->lower_bound(as_view(lower));
    load();
  }

  [[nodiscard]] bool valid() const noexcept override { return valid_; }

  [[nodiscard]] const InternalKey& key() const override {
    if (!valid_ || node_ == nullptr) throw Error("iterator is invalid");
    key_.assign(node_->user_key(), node_->timestamp);
    return key_;
  }

  [[nodiscard]] InternalKeyView key_view() const override {
    if (!valid_ || node_ == nullptr) throw Error("iterator is invalid");
    return node_->view();
  }

  [[nodiscard]] ByteView value() const override {
    if (!valid_) throw Error("iterator is invalid");
    return value_;
  }

  void next() override {
    if (node_ == nullptr) return;
    node_ = SkipList::load(node_->next(0));
    load();
  }

  // Keys and values point into the arena, so rows stay valid for the
  // memtable's lifetime rather than only until the next pull.
  std::size_t pull(std::span<ScanRow> out) override {
    std::size_t n = 0;
    while (n < out.size() && valid_) {
      out[n++] = {node_->view(), value_};
      next();
    }
    return n;
  }

 private:
  void load() {
    valid_ = false;
    value_ = {};
    if (node_ == nullptr) return;
    if (upper_.has_value() &&
        compare_internal(node_->view(), as_view(*upper_)) >= 0) {
      node_ = nullptr;
      return;
    }
    value_ = node_->value_view();
    valid_ = true;
  }

  std::shared_ptr<MemTable> owner_;
  SkipList* list_{};
  SkipList::Node* node_{};
  std::optional<InternalKey> upper_;
  ByteView value_;
  mutable InternalKey key_;
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
