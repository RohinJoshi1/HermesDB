#include "hermesdb/table.hpp"
#include "hermesdb/iterator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cerrno>
#include <climits>
#include <limits>
#include <mutex>
#include <utility>
#include <zlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hermesdb {
namespace {

void encode_key(Bytes& out, const InternalKey& key) {
  const Bytes encoded = key.encode();
  put_u16(out, narrow_size<std::uint16_t>(encoded.size(), "metadata key"));
  out.insert(out.end(), encoded.begin(), encoded.end());
}

InternalKey decode_key(ByteView bytes, std::size_t& cursor,
                       std::size_t limit) {
  const auto size = read_u16(bytes, cursor);
  cursor += 2;
  if (cursor + size > limit) throw Error("truncated table metadata key");
  const auto result = InternalKey::decode(bytes.subspan(cursor, size));
  cursor += size;
  return result;
}

constexpr std::uint32_t kTableMagic = 0x31424448;  // HDB1
constexpr std::uint32_t kTableVersion = 1;
constexpr std::size_t kDeviceBlock = 4096;
constexpr Byte kCodecNone = 0;
constexpr Byte kCodecZlib = 1;

bool is_packed_table(ByteView encoded) noexcept {
  if (encoded.size() < 8) return false;
  return read_u32(encoded, encoded.size() - 8) == kTableMagic &&
         read_u32(encoded, encoded.size() - 4) == kTableVersion;
}

Bytes zlib_compress(ByteView src) {
  if (src.empty()) return {};
  uLong bound = compressBound(static_cast<uLong>(src.size()));
  Bytes dest(bound);
  uLong dest_len = bound;
  const int rc = compress2(reinterpret_cast<Bytef*>(dest.data()), &dest_len,
                           reinterpret_cast<const Bytef*>(src.data()),
                           static_cast<uLong>(src.size()), Z_BEST_SPEED);
  if (rc != Z_OK) throw Error("zlib compress failed");
  dest.resize(dest_len);
  return dest;
}

Bytes zlib_decompress(ByteView src, std::size_t uncompressed_len) {
  Bytes dest(uncompressed_len);
  uLong dest_len = static_cast<uLong>(uncompressed_len);
  const int rc = uncompress(reinterpret_cast<Bytef*>(dest.data()), &dest_len,
                            reinterpret_cast<const Bytef*>(src.data()),
                            static_cast<uLong>(src.size()));
  if (rc != Z_OK || dest_len != uncompressed_len) {
    throw Error("zlib decompress failed");
  }
  return dest;
}

Bytes encode_packed_payload(ByteView uncompressed) {
  Bytes payload;
  payload.reserve(uncompressed.size() + 9);
  Bytes compressed;
  Byte codec = kCodecNone;
  if (!uncompressed.empty()) {
    compressed = zlib_compress(uncompressed);
    if (compressed.size() < uncompressed.size()) {
      codec = kCodecZlib;
    }
  }
  payload.push_back(codec);
  put_u32(payload, narrow_size<std::uint32_t>(uncompressed.size(),
                                              "uncompressed block"));
  if (codec == kCodecZlib) {
    payload.insert(payload.end(), compressed.begin(), compressed.end());
  } else {
    payload.insert(payload.end(), uncompressed.begin(), uncompressed.end());
  }
  put_u32(payload, checksum(uncompressed));
  return payload;
}

Bytes decode_packed_payload(ByteView payload) {
  if (payload.size() < 9) throw Error("truncated packed block");
  const auto codec = payload[0];
  const auto uncompressed_len = read_u32(payload, 1);
  const ByteView body = payload.subspan(5, payload.size() - 9);
  const auto expected = read_u32(payload, payload.size() - 4);
  Bytes uncompressed;
  if (codec == kCodecNone) {
    uncompressed.assign(body.begin(), body.end());
  } else if (codec == kCodecZlib) {
    uncompressed = zlib_decompress(body, uncompressed_len);
  } else {
    throw Error("unknown SST block codec");
  }
  if (uncompressed.size() != uncompressed_len ||
      checksum(uncompressed) != expected) {
    throw Error("table block checksum mismatch");
  }
  return uncompressed;
}

std::vector<BlockMeta> parse_block_meta(ByteView meta_bytes,
                                        std::uint32_t meta_offset,
                                        bool packed) {
  if (meta_bytes.size() < 4) throw Error("truncated table metadata");
  std::size_t cursor = 0;
  const auto metadata_end = meta_bytes.size();
  const auto count = read_u32(meta_bytes, cursor);
  cursor += 4;
  constexpr std::size_t kMinimumMetaSize = 8;
  if (count == 0 ||
      static_cast<std::size_t>(count) > metadata_end / kMinimumMetaSize) {
    throw Error("invalid table block count");
  }
  std::vector<BlockMeta> meta;
  meta.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (cursor + 4 > metadata_end) throw Error("truncated table metadata");
    BlockMeta item;
    item.offset = read_u32(meta_bytes, cursor);
    cursor += 4;
    if (packed) {
      if (cursor + 4 > metadata_end) throw Error("truncated table metadata");
      item.size = read_u32(meta_bytes, cursor);
      cursor += 4;
    }
    item.first_key = decode_key(meta_bytes, cursor, metadata_end);
    item.last_key = decode_key(meta_bytes, cursor, metadata_end);
    const auto block_end =
        item.size == 0 ? meta_offset : item.offset + item.size;
    if (item.offset >= meta_offset || block_end > meta_offset ||
        (!meta.empty() && item.offset <= meta.back().offset) ||
        item.last_key < item.first_key) {
      throw Error("invalid table block metadata");
    }
    meta.push_back(std::move(item));
  }
  if (cursor != metadata_end || meta.empty() || meta.front().offset != 0) {
    throw Error("invalid table metadata");
  }
  return meta;
}

struct TableIndex {
  std::vector<BlockMeta> meta;
  std::uint32_t meta_offset{};
  BloomFilter bloom;
};

TableIndex parse_table_index(ByteView encoded) {
  if (encoded.size() < 25) throw Error("table is too short");
  const bool packed = is_packed_table(encoded);
  const std::size_t footer = packed ? encoded.size() - 8 : encoded.size();
  const auto bloom_offset = read_u32(encoded, footer - 4);
  if (bloom_offset < 8 || bloom_offset + 8 > footer) {
    throw Error("invalid Bloom offset");
  }
  const auto expected_bloom_checksum = read_u32(encoded, footer - 8);
  const ByteView bloom_bytes =
      encoded.subspan(bloom_offset, footer - 8 - bloom_offset);
  if (checksum(bloom_bytes) != expected_bloom_checksum) {
    throw Error("table Bloom checksum mismatch");
  }
  auto bloom = BloomFilter::Decode(bloom_bytes);
  const std::size_t metadata_end = bloom_offset - 8;
  const auto expected_meta_checksum = read_u32(encoded, metadata_end);
  const auto meta_offset = read_u32(encoded, metadata_end + 4);
  if (meta_offset > metadata_end) throw Error("invalid metadata offset");
  const ByteView meta_bytes =
      encoded.subspan(meta_offset, metadata_end - meta_offset);
  if (checksum(meta_bytes) != expected_meta_checksum) {
    throw Error("table metadata checksum mismatch");
  }
  return {parse_block_meta(meta_bytes, meta_offset, packed), meta_offset,
          std::move(bloom)};
}

}  // namespace

class SstFile {
 public:
  explicit SstFile(const std::filesystem::path& path) {
#ifdef _WIN32
    fd_ = ::_wopen(path.wstring().c_str(), _O_RDONLY | _O_BINARY);
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
#endif
    if (fd_ < 0) throw Error("failed to open SST");
#ifdef _WIN32
    const auto end = ::_filelengthi64(fd_);
    if (end < 0) throw Error("failed to stat SST");
    size_ = static_cast<std::uint64_t>(end);
#else
    struct stat st {};
    if (::fstat(fd_, &st) != 0) throw Error("failed to stat SST");
    if (st.st_size < 0) throw Error("failed to stat SST");
    size_ = static_cast<std::uint64_t>(st.st_size);
#endif
  }

  ~SstFile() {
    if (fd_ < 0) return;
#ifdef _WIN32
    ::_close(fd_);
#else
    ::close(fd_);
#endif
  }

  SstFile(const SstFile&) = delete;
  SstFile& operator=(const SstFile&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

  [[nodiscard]] Bytes pread(std::uint64_t offset, std::size_t length) const {
    if (length == 0) return {};
    if (offset > size_ || length > size_ - offset) {
      throw Error("SST read out of range");
    }
    Bytes out(length);
    std::size_t done = 0;
#ifdef _WIN32
    std::lock_guard lock(io_mutex_);
    while (done != length) {
      if (::_lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET) < 0) {
        throw Error("failed to seek SST");
      }
      const auto count = static_cast<unsigned int>(std::min<std::size_t>(
          length - done, std::numeric_limits<unsigned int>::max()));
      const int n = ::_read(fd_, out.data() + done, count);
      if (n < 0) throw Error("failed to read SST");
      if (n == 0) throw Error("SST shortened while reading");
      done += static_cast<std::size_t>(n);
    }
#else
    while (done != length) {
      const auto n = ::pread(fd_, out.data() + done, length - done,
                             static_cast<off_t>(offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        throw Error("failed to read SST");
      }
      if (n == 0) throw Error("SST shortened while reading");
      done += static_cast<std::size_t>(n);
    }
#endif
    return out;
  }

  void advise(std::uint64_t offset, std::size_t length) const {
    if (fd_ < 0 || length == 0) return;
#ifdef __APPLE__
    struct radvisory hint {};
    hint.ra_offset = static_cast<off_t>(offset);
    hint.ra_count = static_cast<int>(
        std::min(length, static_cast<std::size_t>(INT_MAX)));
    ::fcntl(fd_, F_RDADVISE, &hint);
#elif !defined(_WIN32)
    ::posix_fadvise(fd_, static_cast<off_t>(offset),
                    static_cast<off_t>(length), POSIX_FADV_WILLNEED);
#endif
  }

 private:
  int fd_{-1};
  std::uint64_t size_{};
#ifdef _WIN32
  mutable std::mutex io_mutex_;
#endif
};

namespace {

TableIndex parse_table_index_file(const SstFile& file) {
  const auto size = file.size();
  if (size < 25) throw Error("table is too short");
  Bytes tail = file.pread(size - 8, 8);
  const bool packed = read_u32(tail, 0) == kTableMagic &&
                      read_u32(tail, 4) == kTableVersion;
  const std::uint64_t footer = packed ? size - 8 : size;
  if (footer < 8) throw Error("table is too short");
  Bytes bloom_footer = file.pread(footer - 8, 8);
  const auto bloom_offset = read_u32(bloom_footer, 4);
  if (bloom_offset < 8 || bloom_offset + 8 > footer) {
    throw Error("invalid Bloom offset");
  }
  const auto expected_bloom_checksum = read_u32(bloom_footer, 0);
  Bytes bloom_owned =
      file.pread(bloom_offset, static_cast<std::size_t>(footer - 8 - bloom_offset));
  if (checksum(bloom_owned) != expected_bloom_checksum) {
    throw Error("table Bloom checksum mismatch");
  }
  auto bloom = BloomFilter::Decode(bloom_owned);
  const std::uint64_t metadata_end = bloom_offset - 8;
  Bytes meta_footer = file.pread(metadata_end, 8);
  const auto expected_meta_checksum = read_u32(meta_footer, 0);
  const auto meta_offset = read_u32(meta_footer, 4);
  if (meta_offset > metadata_end) throw Error("invalid metadata offset");
  Bytes meta_owned = file.pread(
      meta_offset, static_cast<std::size_t>(metadata_end - meta_offset));
  if (checksum(meta_owned) != expected_meta_checksum) {
    throw Error("table metadata checksum mismatch");
  }
  return {parse_block_meta(meta_owned, meta_offset, packed), meta_offset,
          std::move(bloom)};
}

}  // namespace

BloomFilter BloomFilter::Build(std::span<const std::uint32_t> key_hashes,
                               std::size_t bits_per_key) {
  std::size_t bit_count =
      std::max<std::size_t>(64, key_hashes.size() * bits_per_key);
  bit_count = (bit_count + 7U) & ~std::size_t{7};
  Bytes bits(bit_count / 8U, 0);
  const auto hash_count = static_cast<std::uint8_t>(
      std::clamp<std::size_t>(
          static_cast<std::size_t>(static_cast<double>(bits_per_key) * 0.69),
          1, 30));
  for (auto hash : key_hashes) {
    const auto delta = std::rotl(hash, 15);
    for (std::uint8_t round = 0; round < hash_count; ++round) {
      const auto bit = static_cast<std::size_t>(hash) % bit_count;
      bits[bit / 8U] |= static_cast<Byte>(1U << (bit % 8U));
      hash += delta;
    }
  }
  return BloomFilter(std::move(bits), hash_count);
}

std::size_t BloomFilter::BitsPerKey(std::size_t entries,
                                    double false_positive_rate) {
  if (entries == 0) return 0;
  if (!(false_positive_rate > 0.0 && false_positive_rate < 1.0)) {
    throw Error("Bloom false-positive rate must be between zero and one");
  }
  constexpr double kLn2Squared = 0.4804530139182014;
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(
             std::ceil(-std::log(false_positive_rate) / kLn2Squared)));
}

BloomFilter BloomFilter::Decode(ByteView encoded) {
  if (encoded.empty()) throw Error("empty Bloom filter");
  const auto hash_count = encoded.back();
  if (hash_count == 0 || hash_count > 30) {
    throw Error("invalid Bloom hash count");
  }
  return BloomFilter(Bytes(encoded.begin(), encoded.end() - 1), hash_count);
}

bool BloomFilter::MayContain(std::uint32_t key_hash) const noexcept {
  if (bits_.empty()) return false;
  const auto bit_count = bits_.size() * 8U;
  const auto delta = std::rotl(key_hash, 15);
  for (std::uint8_t round = 0; round < hash_count_; ++round) {
    const auto bit = static_cast<std::size_t>(key_hash) % bit_count;
    if ((bits_[bit / 8U] & static_cast<Byte>(1U << (bit % 8U))) == 0) {
      return false;
    }
    key_hash += delta;
  }
  return true;
}

Bytes BloomFilter::Encode() const {
  Bytes encoded = bits_;
  encoded.push_back(hash_count_);
  return encoded;
}

BlockCache::BlockCache(std::size_t capacity) {
  if (capacity == 0) throw Error("block cache capacity must be positive");
  shard_count_ = 1;
  while (shard_count_ * 2 <= capacity && shard_count_ < 16) shard_count_ *= 2;
  shard_mask_ = shard_count_ - 1;
  per_shard_capacity_ = (capacity + shard_count_ - 1) / shard_count_;
  shards_ = std::make_unique<Shard[]>(shard_count_);
}

std::size_t BlockCache::KeyHash::operator()(const Key& key) const noexcept {
  const auto first = std::hash<std::uint64_t>{}(key.table_id);
  const auto second = std::hash<std::size_t>{}(key.block_index);
  return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
}

BlockCache::Shard& BlockCache::shard_for(const Key& key) {
  return shards_[KeyHash{}(key) & shard_mask_];
}

std::shared_ptr<const Block> BlockCache::Get(std::uint64_t table_id,
                                              std::size_t block_index,
                                              bool promote) {
  const Key key{table_id, block_index};
  auto& shard = shard_for(key);
  std::lock_guard lock(shard.mutex);
  const auto found = shard.index.find(key);
  if (found == shard.index.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  hits_.fetch_add(1, std::memory_order_relaxed);
  if (promote) found->second->point = true;
  shard.entries.splice(shard.entries.begin(), shard.entries, found->second);
  return found->second->block;
}

bool BlockCache::Contains(std::uint64_t table_id, std::size_t block_index) {
  const Key key{table_id, block_index};
  auto& shard = shard_for(key);
  std::lock_guard lock(shard.mutex);
  return shard.index.find(key) != shard.index.end();
}

void BlockCache::Insert(std::uint64_t table_id, std::size_t block_index,
                        std::shared_ptr<const Block> block, bool point) {
  if (!block) throw Error("cannot cache a null block");
  const Key key{table_id, block_index};
  auto& shard = shard_for(key);
  std::lock_guard lock(shard.mutex);
  if (const auto found = shard.index.find(key); found != shard.index.end()) {
    found->second->block = std::move(block);
    found->second->point = found->second->point || point;
    shard.entries.splice(shard.entries.begin(), shard.entries, found->second);
    return;
  }

  auto evict_one = [&](bool scan_only) {
    if (shard.entries.size() < per_shard_capacity_) return true;
    if (!scan_only) {
      shard.index.erase(shard.entries.back().key);
      shard.entries.pop_back();
      return true;
    }
    for (auto it = shard.entries.end(); it != shard.entries.begin();) {
      --it;
      if (!it->point) {
        shard.index.erase(it->key);
        shard.entries.erase(it);
        return true;
      }
    }
    return false;
  };

  if (!evict_one(!point)) return;
  shard.entries.push_front(Entry{key, std::move(block), point});
  shard.index[key] = shard.entries.begin();
}

Table::Table(Bytes bytes, std::unique_ptr<SstFile> file,
             std::vector<BlockMeta> meta, std::uint32_t meta_offset,
             BloomFilter bloom, std::uint64_t file_size)
    : bytes_(std::move(bytes)),
      file_(std::move(file)),
      file_size_(file_size),
      meta_(std::move(meta)),
      meta_offset_(meta_offset),
      bloom_(std::move(bloom)) {}

Table::~Table() = default;

std::shared_ptr<Table> Table::open(Bytes encoded) {
  auto index = parse_table_index(encoded);
  const auto size = static_cast<std::uint64_t>(encoded.size());
  return std::shared_ptr<Table>(new Table(
      std::move(encoded), nullptr, std::move(index.meta), index.meta_offset,
      std::move(index.bloom), size));
}

std::shared_ptr<Table> Table::open(const std::filesystem::path& path) {
  auto file = std::make_unique<SstFile>(path);
  auto index = parse_table_index_file(*file);
  const auto size = file->size();
  return std::shared_ptr<Table>(
      new Table({}, std::move(file), std::move(index.meta), index.meta_offset,
                std::move(index.bloom), size));
}

bool Table::may_contain(ByteView user_key) const noexcept {
  return bloom_.MayContain(checksum(user_key));
}

std::size_t Table::block_span(std::size_t index) const {
  if (index >= meta_.size()) throw Error("table block index out of range");
  const std::size_t begin = meta_[index].offset;
  const std::size_t span =
      meta_[index].size != 0
          ? static_cast<std::size_t>(meta_[index].size)
          : (index + 1 < meta_.size() ? meta_[index + 1].offset
                                      : meta_offset_) -
                begin;
  if (span < 4 ||
      static_cast<std::uint64_t>(begin) + span > file_size_) {
    throw Error("corrupt table block span");
  }
  return span;
}

std::shared_ptr<const Block> Table::decode_stored(std::size_t index,
                                                  ByteView stored) const {
  if (meta_[index].size != 0) {
    return Block::decode(decode_packed_payload(stored));
  }
  const ByteView encoded = stored.subspan(0, stored.size() - 4);
  if (checksum(encoded) != read_u32(stored, stored.size() - 4)) {
    throw Error("table block checksum mismatch");
  }
  return Block::decode(Bytes(encoded.begin(), encoded.end()));
}

std::shared_ptr<const Block> Table::read_block(std::size_t index) const {
  const auto span = block_span(index);
  const std::size_t begin = meta_[index].offset;
  if (file_) {
    const auto owned = file_->pread(begin, span);
    return decode_stored(index, owned);
  }
  return decode_stored(index, ByteView(bytes_).subspan(begin, span));
}

std::shared_ptr<const Block> Table::read_block_cached(
    std::size_t index, std::uint64_t table_id, BlockCache& cache) const {
  if (auto cached = cache.Get(table_id, index, true)) return cached;
  auto block = read_block(index);
  cache.Insert(table_id, index, block, true);
  return block;
}

constexpr std::size_t kScanPrefetchBlocks = 4;

std::shared_ptr<const Block> Table::fill_scan_window(
    std::size_t start, std::size_t count, BlockCache* cache,
    std::uint64_t table_id) const {
  if (cache == nullptr || count == 0 || start >= meta_.size()) return {};
  const std::size_t end = std::min(meta_.size(), start + count);
  const std::size_t first = meta_[start].offset;
  const std::size_t last = meta_[end - 1].offset;
  const std::size_t total = last + block_span(end - 1) - first;
  std::shared_ptr<const Block> head;
  if (file_ != nullptr) {
    file_->advise(first, total);
    const auto owned = file_->pread(first, total);
    const ByteView window = owned;
    for (std::size_t index = start; index < end; ++index) {
      if (index != start && cache->Contains(table_id, index)) continue;
      const auto block = decode_stored(
          index, window.subspan(meta_[index].offset - first, block_span(index)));
      cache->Insert(table_id, index, block, false);
      if (index == start) head = block;
    }
    return head;
  }
  for (std::size_t index = start; index < end; ++index) {
    if (index != start && cache->Contains(table_id, index)) continue;
    auto block = read_block(index);
    cache->Insert(table_id, index, block, false);
    if (index == start) head = std::move(block);
  }
  return head;
}

std::shared_ptr<const Block> Table::read_block_for_scan(
    std::size_t index, BlockCache* cache, std::uint64_t table_id) const {
  if (cache == nullptr) return read_block(index);
  if (auto cached = cache->Get(table_id, index)) return cached;
  if (auto filled = fill_scan_window(index, 1 + kScanPrefetchBlocks, cache,
                                     table_id)) {
    return filled;
  }
  auto block = read_block(index);
  cache->Insert(table_id, index, block, false);
  return block;
}

void Table::prefetch_scan_blocks(std::size_t after, std::size_t window,
                                 BlockCache* cache,
                                 std::uint64_t table_id) const {
  if (after + 1 >= meta_.size()) return;
  static_cast<void>(fill_scan_window(after + 1, window, cache, table_id));
}

std::size_t Table::find_block(const InternalKey& key) const {
  const auto it = std::lower_bound(
      meta_.begin(), meta_.end(), key,
      [](const BlockMeta& item, const InternalKey& target) {
        return item.last_key < target;
      });
  return it == meta_.end() ? meta_.size() - 1
                           : static_cast<std::size_t>(it - meta_.begin());
}

std::optional<Bytes> Table::get(ByteView user_key) const {
  return get(user_key, kMaxTimestamp);
}

std::optional<Bytes> Table::get(ByteView user_key,
                                std::uint64_t read_timestamp,
                                BlockCache* cache,
                                std::uint64_t table_id) const {
  if (bytes_less(user_key, meta_.front().first_key.user_key()) ||
      bytes_less(meta_.back().last_key.user_key(), user_key) ||
      !may_contain(user_key)) {
    return std::nullopt;
  }
  const InternalKey target(Bytes(user_key.begin(), user_key.end()),
                           read_timestamp);
  const auto block_index = find_block(target);
  const auto block = cache ? read_block_cached(block_index, table_id, *cache)
                           : read_block(block_index);
  BlockIterator iterator(block);
  iterator.seek(target);
  if (!iterator.valid() ||
      !std::equal(iterator.key().user_key().begin(),
                  iterator.key().user_key().end(), user_key.begin(),
                  user_key.end()))
    return std::nullopt;
  return Bytes(iterator.value().begin(), iterator.value().end());
}

class TableCursor final : public StorageIterator {
 public:
  TableCursor(std::shared_ptr<const Table> table, const InternalKey& lower,
              std::optional<InternalKey> upper, BlockCache* cache,
              std::uint64_t table_id)
      : table_(std::move(table)),
        upper_(std::move(upper)),
        cache_(cache),
        table_id_(table_id) {
    if (table_->num_blocks() == 0) return;
    block_index_ = table_->find_block(lower);
    load_block(true, lower);
  }

  [[nodiscard]] bool valid() const noexcept override {
    return valid_ && block_iter_ != nullptr &&
           (block_iter_->valid() ||
            block_index_ + 1 < table_->num_blocks());
  }

  [[nodiscard]] const InternalKey& key() const override {
    if (!valid_ || block_iter_ == nullptr) throw Error("iterator is invalid");
    return block_iter_->key();
  }

  [[nodiscard]] InternalKeyView key_view() const override {
    if (!valid_ || block_iter_ == nullptr) throw Error("iterator is invalid");
    return block_iter_->key_view();
  }

  [[nodiscard]] ByteView value() const override {
    if (!valid_ || block_iter_ == nullptr) throw Error("iterator is invalid");
    return block_iter_->value();
  }

  void next() override {
    if (!valid_ || block_iter_ == nullptr) return;
    block_iter_->next();
    if (accept_current()) return;
    ++block_index_;
    load_block(false, InternalKey{});
  }

  void skip_current_user() override {
    if (!valid_ || block_iter_ == nullptr) return;
    const auto parked = block_iter_->key_view();
    const auto pin = block_;
    next();
    while (valid_ && same_user(key_view(), parked)) next();
    static_cast<void>(pin);
  }

  std::size_t pull(std::span<ScanRow> out) override {
    if (!valid_ || block_iter_ == nullptr) return 0;
    auto got = block_iter_->pull(out);
    if (got == 0) {
      ++block_index_;
      load_block(false, InternalKey{});
      if (!valid_ || block_iter_ == nullptr) return 0;
      got = block_iter_->pull(out);
    }
    if (got == 0) return 0;
    if (!upper_.has_value()) return got;
    std::size_t keep = 0;
    while (keep < got &&
           compare_internal(out[keep].key, as_view(*upper_)) < 0) {
      ++keep;
    }
    if (keep != got) valid_ = false;
    return keep;
  }

 private:
  [[nodiscard]] bool accept_current() const {
    if (block_iter_ == nullptr || !block_iter_->valid()) return false;
    if (!upper_.has_value()) return true;
    return compare_internal(block_iter_->key_view(), as_view(*upper_)) < 0;
  }

  void load_block(bool seek, const InternalKey& target) {
    valid_ = false;
    block_iter_.reset();
    while (block_index_ < table_->num_blocks()) {
      block_ = table_->read_block_for_scan(block_index_, cache_, table_id_);
      block_iter_ = std::make_unique<BlockIterator>(block_);
      if (seek) {
        block_iter_->seek(target);
      }
      if (accept_current()) {
        valid_ = true;
        return;
      }
      ++block_index_;
      seek = false;
    }
  }

  std::shared_ptr<const Table> table_;
  std::optional<InternalKey> upper_;
  BlockCache* cache_{};
  std::uint64_t table_id_{};
  std::size_t block_index_{0};
  std::shared_ptr<const Block> block_;
  std::unique_ptr<BlockIterator> block_iter_;
  bool valid_{false};
};

IteratorPtr Table::iter() const {
  if (meta_.empty()) {
    return std::make_unique<VectorIterator>(std::vector<KeyValue>{});
  }
  return iter_scan(meta_.front().first_key, std::nullopt);
}

IteratorPtr Table::iter_from(const InternalKey& key) const {
  if (meta_.empty()) {
    return std::make_unique<VectorIterator>(std::vector<KeyValue>{});
  }
  return iter_scan(key, std::nullopt);
}

IteratorPtr Table::iter_scan(const InternalKey& lower,
                             std::optional<InternalKey> upper, BlockCache* cache,
                             std::uint64_t table_id) const {
  return std::make_unique<TableCursor>(shared_from_this(), lower, upper, cache,
                                       table_id);
}

TableBuilder::TableBuilder(std::size_t block_size, Compression compression)
    : block_size_(block_size),
      compression_(compression),
      block_(block_size) {}

void TableBuilder::pad_pack(std::size_t extra) {
  if (compression_ != Compression::zlib || extra == 0) return;
  const auto used = data_.size() % kDeviceBlock;
  if (used == 0) return;
  const auto room = kDeviceBlock - used;
  if (extra > kDeviceBlock) {
    data_.resize(data_.size() + room, 0);
    return;
  }
  if (extra > room) {
    data_.resize(data_.size() + room, 0);
  }
}

bool TableBuilder::empty() const noexcept {
  return meta_.empty() && block_.empty();
}

void TableBuilder::add(const InternalKey& key, ByteView value) {
  if (previous_key_ && !(previous_key_.value() < key)) {
    throw Error("table keys must be added in strictly increasing order");
  }
  if (!block_.add(key, value)) {
    finish_block();
    if (!block_.add(key, value)) throw Error("failed to add to empty block");
  }
  if (!first_key_) first_key_ = key;
  last_key_ = key;
  previous_key_ = key;
  key_hashes_.push_back(checksum(key.user_key()));
}

void TableBuilder::finish_block() {
  if (block_.empty()) return;
  const Bytes encoded = block_.finish()->encode();
  raw_block_bytes_ += encoded.size();
  Bytes payload;
  std::uint32_t size = 0;
  if (compression_ == Compression::zlib) {
    payload = encode_packed_payload(encoded);
    size = narrow_size<std::uint32_t>(payload.size(), "packed block");
    pad_pack(payload.size());
  } else {
    payload = encoded;
    put_u32(payload, checksum(encoded));
  }
  const auto offset = narrow_size<std::uint32_t>(data_.size(), "table");
  data_.insert(data_.end(), payload.begin(), payload.end());
  meta_.push_back(BlockMeta{offset, size, *first_key_, *last_key_});
  block_ = BlockBuilder(block_size_);
  first_key_.reset();
  last_key_.reset();
}

Bytes TableBuilder::finish() {
  finish_block();
  if (meta_.empty()) throw Error("cannot finish an empty table");
  const auto meta_offset = narrow_size<std::uint32_t>(data_.size(), "table");
  Bytes metadata;
  put_u32(metadata, narrow_size<std::uint32_t>(meta_.size(), "block count"));
  for (const auto& item : meta_) {
    put_u32(metadata, item.offset);
    if (compression_ == Compression::zlib) put_u32(metadata, item.size);
    encode_key(metadata, item.first_key);
    encode_key(metadata, item.last_key);
  }
  data_.insert(data_.end(), metadata.begin(), metadata.end());
  put_u32(data_, checksum(metadata));
  put_u32(data_, meta_offset);
  const auto bloom_offset = narrow_size<std::uint32_t>(data_.size(), "table");
  auto bloom = BloomFilter::Build(
      key_hashes_, BloomFilter::BitsPerKey(key_hashes_.size()));
  const auto encoded_bloom = bloom.Encode();
  data_.insert(data_.end(), encoded_bloom.begin(), encoded_bloom.end());
  put_u32(data_, checksum(encoded_bloom));
  put_u32(data_, bloom_offset);
  if (compression_ == Compression::zlib) {
    put_u32(data_, kTableMagic);
    put_u32(data_, kTableVersion);
  }
  return std::move(data_);
}

}  // namespace hermesdb
