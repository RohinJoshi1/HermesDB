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

std::size_t VectorIterator::pull(std::span<ScanRow> out) {
  std::size_t n = 0;
  while (n < out.size() && valid()) {
    out[n++] = {as_view(entries_[index_].first), entries_[index_].second};
    ++index_;
  }
  return n;
}

MergeIterator::MergeIterator(std::vector<IteratorPtr> children)
    : children_(std::move(children)), batches_(children_.size()) {
  for (std::size_t i = 0; i < children_.size(); ++i) refill_child(i);
}

bool MergeIterator::valid() const noexcept {
  for (std::size_t i = 0; i < children_.size(); ++i) {
    if (!batches_[i].empty() || children_[i]->valid()) return true;
  }
  return false;
}

void MergeIterator::prime() {
  for (std::size_t i = 0; i < children_.size(); ++i) {
    if (batches_[i].empty()) refill_child(i);
  }
}

std::size_t MergeIterator::pick() const {
  std::size_t best = children_.size();
  for (std::size_t i = 0; i < children_.size(); ++i) {
    if (batches_[i].empty()) continue;
    if (best == children_.size() || ahead(i, best)) best = i;
  }
  return best;
}

const InternalKey& MergeIterator::key() const {
  const_cast<MergeIterator*>(this)->prime();
  const auto view = child_key(pick());
  Bytes user;
  view.materialize_user(user);
  key_scratch_.assign(user, view.timestamp());
  return key_scratch_;
}

InternalKeyView MergeIterator::key_view() const {
  const_cast<MergeIterator*>(this)->prime();
  return child_key(pick());
}

ByteView MergeIterator::value() const {
  const_cast<MergeIterator*>(this)->prime();
  return child_row(pick()).value;
}

InternalKeyView MergeIterator::child_key(std::size_t child) const {
  return child_row(child).key;
}

ScanRow MergeIterator::child_row(std::size_t child) const {
  const auto& batch = batches_[child];
  if (batch.empty()) throw Error("iterator is invalid");
  return batch.rows[batch.pos];
}

bool MergeIterator::refill_child(std::size_t child) {
  auto& batch = batches_[child];
  std::array<ScanRow, kScanBatch> raw{};
  const auto n = children_[child]->pull(raw);
  batch.pos = 0;
  batch.size = static_cast<std::uint8_t>(n);
  for (std::uint8_t i = 0; i < batch.size; ++i) {
    raw[i].key.materialize_user(batch.keys[i]);
    batch.vals[i].assign(raw[i].value.begin(), raw[i].value.end());
    batch.rows[i] = {InternalKeyView(batch.keys[i], raw[i].key.timestamp()),
                     batch.vals[i]};
  }
  return !batch.empty();
}

void MergeIterator::advance_child(std::size_t child) {
  auto& batch = batches_[child];
  if (!batch.empty()) ++batch.pos;
  if (batch.empty()) refill_child(child);
}

bool MergeIterator::ahead(std::size_t a, std::size_t b) const {
  const auto order = compare_internal(child_key(a), child_key(b));
  if (order != 0) return order < 0;
  return a < b;
}

void MergeIterator::next() {
  prime();
  const auto child = pick();
  if (child == children_.size()) return;
  if (children_.size() == 1) {
    advance_child(child);
    return;
  }
  const auto parked = child_key(child);
  parked.materialize_user(previous_user_);
  previous_timestamp_ = parked.timestamp();
  while (pick() != children_.size()) {
    const auto current = pick();
    const auto view = child_key(current);
    if (!same_user(view, previous_user_) ||
        view.timestamp() != previous_timestamp_) {
      break;
    }
    advance_child(current);
  }
}

void MergeIterator::skip_current_user() {
  prime();
  const auto first = pick();
  if (first == children_.size()) return;
  child_key(first).materialize_user(previous_user_);
  for (std::size_t i = 0; i < children_.size(); ++i) {
    while (!batches_[i].empty()) {
      auto& batch = batches_[i];
      const auto n = leading_same_user(
          std::span<const ScanRow>{
              batch.rows.data() + batch.pos,
              static_cast<std::size_t>(batch.size - batch.pos)},
          previous_user_);
      batch.pos = static_cast<std::uint8_t>(batch.pos + n);
      if (!batch.empty()) break;
      refill_child(i);
    }
  }
}

std::size_t MergeIterator::pull(std::span<ScanRow> out) {
  std::size_t n = 0;
  while (n < out.size()) {
    prime();
    const auto child = pick();
    if (child == children_.size()) break;
    const auto row = child_row(child);
    row.key.materialize_user(out_keys_[n]);
    out_vals_[n].assign(row.value.begin(), row.value.end());
    out[n] = {InternalKeyView(out_keys_[n], row.key.timestamp()), out_vals_[n]};
    ++n;
    ++batches_[child].pos;
    if (batches_[child].empty()) refill_child(child);
  }
  return n;
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

std::size_t RangeIterator::pull(std::span<ScanRow> out) {
  if (!valid()) return 0;
  const auto n = child_->pull(out);
  std::size_t keep = 0;
  while (keep < n) {
    if (upper_) {
      const auto order = compare_internal(out[keep].key, as_view(*upper_));
      if (order > 0 || (!upper_inclusive_ && order == 0)) break;
    }
    ++keep;
  }
  return keep;
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
