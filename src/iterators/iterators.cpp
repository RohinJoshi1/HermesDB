#include "hermesdb/iterator.hpp"

#include <algorithm>

namespace hermesdb {

VectorIterator::VectorIterator(std::vector<KeyValue> entries)
    : entries_(std::move(entries)) {}

bool VectorIterator::valid() const noexcept { return index_ < entries_.size(); }

const InternalKey& VectorIterator::key() const {
  if (!valid()) throw Error("iterator is invalid");
  return entries_[index_].first;
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
  select_current();
}

bool MergeIterator::valid() const noexcept { return current_.has_value(); }

const InternalKey& MergeIterator::key() const {
  if (!current_) throw Error("iterator is invalid");
  return children_[*current_]->key();
}

ByteView MergeIterator::value() const {
  if (!current_) throw Error("iterator is invalid");
  return children_[*current_]->value();
}

void MergeIterator::next() {
  if (!current_) return;
  const InternalKey previous = children_[*current_]->key();
  for (auto& child : children_) {
    while (child->valid() && child->key() == previous) child->next();
  }
  select_current();
}

void MergeIterator::select_current() {
  current_.reset();
  for (std::size_t i = 0; i < children_.size(); ++i) {
    if (!children_[i]->valid()) continue;
    if (!current_ || children_[i]->key() < children_[*current_]->key()) {
      current_ = i;
    }
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

ByteView RangeIterator::value() const {
  if (!valid()) throw Error("iterator is invalid");
  return child_->value();
}

void RangeIterator::next() {
  if (valid()) child_->next();
}

void RangeIterator::skip_to_lower() {
  if (!lower_) return;
  while (child_->valid() &&
         (child_->key() < *lower_ ||
          (!lower_inclusive_ && child_->key() == *lower_))) {
    child_->next();
  }
}

bool RangeIterator::within_upper() const noexcept {
  if (!upper_ || !child_->valid()) return true;
  return child_->key() < *upper_ ||
         (upper_inclusive_ && child_->key() == *upper_);
}

}  // namespace hermesdb
