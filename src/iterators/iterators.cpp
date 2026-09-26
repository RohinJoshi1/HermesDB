#include "hermesdb/iterator.hpp"

#include <utility>

namespace hermesdb {

VectorIterator::VectorIterator(std::vector<KeyValue> entries)
    : entries_(std::move(entries)) {}

bool VectorIterator::valid() const noexcept { return index_ < entries_.size(); }

const InternalKey& VectorIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return entries_[index_].first;
}

InternalKeyView VectorIterator::key_view() const {
  return as_view(key());
}

void StorageIterator::skip_current_user() {
  if (!valid()) return;
  const auto parked = key_view();
  next();
  while (valid() && same_user(key_view(), parked)) next();
}

ByteView VectorIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return entries_[index_].second;
}

void VectorIterator::next() {
  if (valid()) ++index_;
}

MergeIterator::MergeIterator(std::vector<IteratorPtr> children)
    : children_(std::move(children)) {
  heap_.reserve(children_.size());
  for (std::size_t i = 0; i < children_.size(); ++i) push(i);
}

bool MergeIterator::valid() const noexcept { return !heap_.empty(); }

std::size_t MergeIterator::top() const {
  if (heap_.empty()) throw Error("iterator is invalid");
  return heap_.front();
}

const InternalKey& MergeIterator::key() const { return children_[top()]->key(); }

InternalKeyView MergeIterator::key_view() const {
  return children_[top()]->key_view();
}

ByteView MergeIterator::value() const { return children_[top()]->value(); }

bool MergeIterator::ahead(std::size_t a, std::size_t b) const {
  const auto order =
      compare_internal(children_[a]->key_view(), children_[b]->key_view());
  if (order != 0) return order < 0;
  return a < b;
}

void MergeIterator::sift_up(std::size_t index) {
  while (index > 0) {
    const std::size_t parent = (index - 1) / 2;
    if (!ahead(heap_[index], heap_[parent])) return;
    std::swap(heap_[index], heap_[parent]);
    index = parent;
  }
}

void MergeIterator::sift_down(std::size_t index) {
  for (;;) {
    std::size_t best = index;
    const std::size_t left = index * 2 + 1;
    const std::size_t right = left + 1;
    if (left < heap_.size() && ahead(heap_[left], heap_[best])) best = left;
    if (right < heap_.size() && ahead(heap_[right], heap_[best])) best = right;
    if (best == index) return;
    std::swap(heap_[index], heap_[best]);
    index = best;
  }
}

void MergeIterator::push(std::size_t child) {
  if (child >= children_.size() || !children_[child]->valid()) return;
  heap_.push_back(child);
  sift_up(heap_.size() - 1);
}

void MergeIterator::pop() {
  heap_.front() = heap_.back();
  heap_.pop_back();
  if (!heap_.empty()) sift_down(0);
}

void MergeIterator::next() {
  if (heap_.empty()) return;
  if (heap_.size() == 1) {
    auto* child = children_[heap_.front()].get();
    child->next();
    if (!child->valid()) heap_.clear();
    return;
  }
  const auto parked = children_[heap_.front()]->key_view();
  parked.materialize_user(previous_user_);
  previous_timestamp_ = parked.timestamp();
  while (!heap_.empty()) {
    const auto current = children_[heap_.front()]->key_view();
    if (!same_user(current, previous_user_) ||
        current.timestamp() != previous_timestamp_) {
      break;
    }
    const auto child = heap_.front();
    pop();
    children_[child]->next();
    push(child);
  }
}

void MergeIterator::skip_current_user() {
  if (heap_.empty()) return;
  if (heap_.size() == 1) {
    auto* child = children_[heap_.front()].get();
    child->skip_current_user();
    if (!child->valid()) heap_.clear();
    return;
  }
  children_[heap_.front()]->key_view().materialize_user(previous_user_);
  while (!heap_.empty() &&
         same_user(children_[heap_.front()]->key_view(), previous_user_)) {
    const auto child = heap_.front();
    pop();
    children_[child]->skip_current_user();
    push(child);
  }
}

RangeIterator::RangeIterator(IteratorPtr child,
                             std::optional<InternalKey> lower,
                             bool lower_inclusive,
                             std::optional<InternalKey> upper,
                             bool upper_inclusive)
    : child_(std::move(child)),
      lower_(std::move(lower)),
      lower_inclusive_(lower_inclusive),
      upper_(std::move(upper)),
      upper_inclusive_(upper_inclusive) {
  if (!child_) throw Error("range iterator requires a child");
  skip_to_lower();
}

bool RangeIterator::valid() const noexcept {
  return child_->valid() && within_upper();
}

const InternalKey& RangeIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return child_->key();
}

InternalKeyView RangeIterator::key_view() const {
  if (!valid()) throw Error("iterator is invalid");
  return child_->key_view();
}

ByteView RangeIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return child_->value();
}

void RangeIterator::next() {
  if (valid()) child_->next();
}

void RangeIterator::skip_to_lower() {
  if (!lower_) return;
  const auto bound = as_view(*lower_);
  while (child_->valid()) {
    const auto order = compare_internal(child_->key_view(), bound);
    if (order > 0 || (lower_inclusive_ && order == 0)) return;
    child_->next();
  }
}

bool RangeIterator::within_upper() const noexcept {
  if (!upper_ || !child_->valid()) return true;
  const auto order = compare_internal(child_->key_view(), as_view(*upper_));
  return order < 0 || (upper_inclusive_ && order == 0);
}

}  // namespace hermesdb
