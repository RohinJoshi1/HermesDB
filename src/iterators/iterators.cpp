#include "tiny_lsm/iterator.hpp"

namespace tiny_lsm {

VectorIterator::VectorIterator(std::vector<KeyValue> entries)
    : entries_(std::move(entries)) {


}

bool VectorIterator::valid() const noexcept {
  return index_ < entries_.size(); 
}

ByteView VectorIterator::key() const {
  if (!valid()) {
    throw Error("iterator is invalid");
  }
  return entries_[index_].first;
}

ByteView VectorIterator::value() const {
  if (!valid()) {
    throw Error("iterator is invalid");
  }
  return entries_[index_].second;
}

void VectorIterator::next() {

  if(valid()){
    index_++;
  }
}

MergeIterator::MergeIterator(std::vector<IteratorPtr> children)
    : children_(std::move(children)) {
  select_current();

}

bool MergeIterator::valid() const noexcept {
  return current_.has_value();
}

ByteView MergeIterator::key() const {
  if(!valid()){
    throw Error("iterator is invalid");
  }
  return children_[current_.value()]->key();
}

ByteView MergeIterator::value() const {

  if(!valid()){
    throw Error("iterator is invalid");
  }
  return children_[current_.value()]->value();
}

void MergeIterator::next() {
  if(!valid()){
    return;
  }
  const auto current_view = key(); 
  Bytes current_key(current_view.begin(), current_view.end());
  for(size_t i = 0; i < children_.size(); i++){
    if(children_[i]->valid() && bytes_equal(children_[i]->key(), current_key)){
      children_[i]->next();
    }
  }
  select_current();
}

void MergeIterator::select_current() {
  current_.reset();

for (std::size_t i = 0; i < children_.size(); ++i) {
  if (!children_[i]->valid()) continue;

  if (!current_ ||
      bytes_less(children_[i]->key(), children_[*current_]->key())) {
    current_ = i;
  }
}
}

RangeIterator::RangeIterator(IteratorPtr child, std::optional<Bytes> lower,
                             bool lower_inclusive,
                             std::optional<Bytes> upper,
                             bool upper_inclusive)
    : child_(std::move(child)),
      lower_(std::move(lower)),
      lower_inclusive_(lower_inclusive),
      upper_(std::move(upper)),
      upper_inclusive_(upper_inclusive) {
  if(!child_){
    throw Error("child is null");
  }
  skip_to_lower();
}

bool RangeIterator::valid() const noexcept {
  return child_ && child_->valid() && within_upper();
}

ByteView RangeIterator::key() const {
  if(!valid()){ 
    throw Error("iterator is invalid"); 
  }
  return child_->key();
}

ByteView RangeIterator::value() const {
  if(!valid()){ 
    throw Error("iterator is invalid"); 
  }
  return child_->value();
}

void RangeIterator::next() {
  if(!valid()){ 
    return; 
  }
  child_->next(); 
}

void RangeIterator::skip_to_lower() {
  if (!lower_) return;

  while (child_->valid() &&
         (bytes_less(child_->key(), *lower_) ||
          (!lower_inclusive_ && bytes_equal(child_->key(), *lower_)))) {
    child_->next();
  }
}

bool RangeIterator::within_upper() const noexcept {
  if (!child_->valid()) return false;
  if (!upper_) return true;
  if (bytes_less(child_->key(), *upper_)) return true;
  return upper_inclusive_ && bytes_equal(child_->key(), *upper_);
}

}  // namespace tiny_lsm
