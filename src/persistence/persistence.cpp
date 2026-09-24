#include "hermesdb/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hermesdb::persistence {
namespace {

constexpr std::size_t kDirectIoAlign = 4096;

#ifdef _WIN32
constexpr int kBinary = _O_BINARY;
int close_fd(int fd) { return ::_close(fd); }
int sync_fd(int fd) { return ::_commit(fd); }
int truncate_fd(int fd, std::uint64_t size) {
  const errno_t error = ::_chsize_s(fd, size);
  if (error == 0) return 0;
  errno = error;
  return -1;
}
int open_new(const std::filesystem::path& path, bool /*direct*/) {
  return ::_wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | kBinary,
                  _S_IREAD | _S_IWRITE);
}
int open_truncated(const std::filesystem::path& path, bool /*direct*/) {
  return ::_wopen(path.c_str(), _O_CREAT | _O_TRUNC | _O_RDWR | kBinary,
                  _S_IREAD | _S_IWRITE);
}
int open_existing(const std::filesystem::path& path, bool /*direct*/) {
  return ::_wopen(path.c_str(), _O_RDWR | kBinary);
}
using IoCount = unsigned int;
#else
int close_fd(int fd) { return ::close(fd); }
int sync_fd(int fd) {
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC, 0) == 0) return 0;
#endif
#ifdef __linux__
  return ::fdatasync(fd);
#else
  return ::fsync(fd);
#endif
}
int truncate_fd(int fd, std::uint64_t size) {
  if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    errno = EFBIG;
    return -1;
  }
  return ::ftruncate(fd, static_cast<off_t>(size));
}

int open_with_bypass(const std::filesystem::path& path, int flags, int mode) {
#if defined(O_DIRECT) && defined(__linux__)
  const int direct = ::open(path.c_str(), flags | O_DIRECT, mode);
  if (direct >= 0 || (errno != EINVAL && errno != EOPNOTSUPP)) return direct;
#endif
  return ::open(path.c_str(), flags, mode);
}

int open_new(const std::filesystem::path& path, bool direct) {
  const int flags = O_CREAT | O_EXCL | O_RDWR;
  return direct ? open_with_bypass(path, flags, 0644)
                : ::open(path.c_str(), flags, 0644);
}
int open_truncated(const std::filesystem::path& path, bool direct) {
  const int flags = O_CREAT | O_TRUNC | O_RDWR;
  return direct ? open_with_bypass(path, flags, 0644)
                : ::open(path.c_str(), flags, 0644);
}
int open_existing(const std::filesystem::path& path, bool direct) {
  const int flags = O_RDWR;
  return direct ? open_with_bypass(path, flags, 0)
                : ::open(path.c_str(), flags);
}

bool fd_is_direct(int fd) {
#if defined(O_DIRECT)
  const int flags = ::fcntl(fd, F_GETFL);
  return flags >= 0 && (flags & O_DIRECT) != 0;
#else
  static_cast<void>(fd);
  return false;
#endif
}

void set_direct_flag(int fd, bool enable) {
#if defined(O_DIRECT)
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0) return;
  const int next = enable ? (flags | O_DIRECT) : (flags & ~O_DIRECT);
  if (next != flags) static_cast<void>(::fcntl(fd, F_SETFL, next));
#else
  static_cast<void>(fd);
  static_cast<void>(enable);
#endif
}

void disable_page_cache(int fd) {
#ifdef F_NOCACHE
  static_cast<void>(::fcntl(fd, F_NOCACHE, 1));
#else
  static_cast<void>(fd);
#endif
}

std::uint64_t fd_size(int fd) {
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    throw std::system_error(errno, std::generic_category(), "stat persistence file");
  }
  return static_cast<std::uint64_t>(info.st_size);
}

std::size_t round_up(std::size_t value, std::size_t align) {
  return ((value + align - 1U) / align) * align;
}

std::unique_ptr<std::byte, void (*)(void*)> allocate_aligned(std::size_t align,
                                                             std::size_t size) {
  void* pointer = nullptr;
  if (size == 0) {
    return {nullptr, std::free};
  }
  if (::posix_memalign(&pointer, align, size) != 0) {
    throw std::bad_alloc();
  }
  return {static_cast<std::byte*>(pointer), std::free};
}

void drop_cached_pages(int fd) {
#ifdef POSIX_FADV_DONTNEED
  static_cast<void>(::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
#else
  static_cast<void>(fd);
#endif
}

using IoCount = std::size_t;
#endif

[[noreturn]] void system_failure(std::string_view operation) {
  throw std::system_error(errno, std::generic_category(), std::string(operation));
}

class File {
 public:
  File() = default;
  File(int fd, bool direct, std::uint64_t size)
      : fd_(fd), offset_(size), direct_(direct) {}
  ~File() {
    if (fd_ >= 0) close_fd(fd_);
  }
  File(File&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        offset_(std::exchange(other.offset_, 0)),
        direct_(std::exchange(other.direct_, false)) {}
  File& operator=(File&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) close_fd(fd_);
      fd_ = std::exchange(other.fd_, -1);
      offset_ = std::exchange(other.offset_, 0);
      direct_ = std::exchange(other.direct_, false);
    }
    return *this;
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  static File create(const std::filesystem::path& path) {
    return adopt(open_new(path, true), "create persistence file", 0);
  }
  static File create_truncated(const std::filesystem::path& path) {
    return adopt(open_truncated(path, true), "create persistence file", 0);
  }
  static File open(const std::filesystem::path& path) {
    const int fd = open_existing(path, true);
    if (fd < 0) system_failure("open persistence file");
#ifdef _WIN32
    const auto size = file_size_win(fd);
#else
    const auto size = fd_size(fd);
    disable_page_cache(fd);
#endif
    return File(fd, fd_is_direct(fd), size);
  }

  [[nodiscard]] Bytes read_all() {
    configure_direct(false);
#ifdef _WIN32
    if (::_lseeki64(fd_, 0, SEEK_SET) < 0) system_failure("seek file");
    const auto end = file_size_win(fd_);
#else
    if (::lseek(fd_, 0, SEEK_SET) < 0) system_failure("seek file");
    const auto end = fd_size(fd_);
#endif
    if (end > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      throw std::length_error("persistence file is too large");
    }
    Bytes result(static_cast<std::size_t>(end));
    std::size_t done = 0;
    while (done != result.size()) {
#ifdef _WIN32
      const auto count = static_cast<IoCount>(
          std::min<std::size_t>(result.size() - done,
                                std::numeric_limits<IoCount>::max()));
      const int n = ::_read(fd_, result.data() + done, count);
#else
      const auto n = ::read(fd_, result.data() + done, result.size() - done);
#endif
      if (n < 0) {
        if (errno == EINTR) continue;
        system_failure("read persistence file");
      }
      if (n == 0) throw std::runtime_error("persistence file shortened while reading");
      done += static_cast<std::size_t>(n);
    }
    offset_ = end;
    configure_direct(direct_);
    return result;
  }

  void write_all(std::span<const std::byte> bytes) {
    if (bytes.empty()) return;
#ifndef _WIN32
    if (direct_) {
      write_direct(bytes);
      return;
    }
#endif
    write_buffered(bytes);
    offset_ += bytes.size();
  }
  void sync() {
    if (sync_fd(fd_) != 0) system_failure("sync persistence file");
#ifndef _WIN32
    if (!direct_) drop_cached_pages(fd_);
#endif
  }
  void truncate(std::size_t size) {
    if (truncate_fd(fd_, size) != 0) system_failure("truncate persistence file");
    offset_ = size;
    sync();
  }

 private:
  static File adopt(int fd, const char* operation, std::uint64_t size) {
    if (fd < 0) system_failure(operation);
#ifndef _WIN32
    disable_page_cache(fd);
    return File(fd, fd_is_direct(fd), size);
#else
    static_cast<void>(size);
    return File(fd, false, 0);
#endif
  }

#ifdef _WIN32
  static std::uint64_t file_size_win(int fd) {
    const __int64 end = ::_lseeki64(fd, 0, SEEK_END);
    if (end < 0 || ::_lseeki64(fd, 0, SEEK_SET) < 0) system_failure("seek file");
    return static_cast<std::uint64_t>(end);
  }
  static bool fd_is_direct(int) { return false; }
#endif

  void configure_direct(bool enable) {
#ifndef _WIN32
    if (direct_) set_direct_flag(fd_, enable);
#else
    static_cast<void>(enable);
#endif
  }

  void write_buffered(std::span<const std::byte> bytes) {
#ifdef _WIN32
    if (::_lseeki64(fd_, static_cast<__int64>(offset_), SEEK_SET) < 0) {
      system_failure("seek persistence file");
    }
#endif
    std::size_t done = 0;
    while (done != bytes.size()) {
#ifdef _WIN32
      const auto count = static_cast<IoCount>(
          std::min<std::size_t>(bytes.size() - done,
                                std::numeric_limits<IoCount>::max()));
      const int n = ::_write(fd_, bytes.data() + done, count);
#else
      const auto n =
          ::pwrite(fd_, bytes.data() + done, bytes.size() - done,
                   static_cast<off_t>(offset_ + done));
#endif
      if (n < 0) {
        if (errno == EINTR) continue;
        system_failure("append persistence file");
      }
      if (n == 0) throw std::runtime_error("zero-byte persistence write");
      done += static_cast<std::size_t>(n);
    }
  }

#ifndef _WIN32
  void write_direct(std::span<const std::byte> bytes) {
    const std::size_t logical = static_cast<std::size_t>(offset_);
    const std::size_t start = logical - (logical % kDirectIoAlign);
    const std::size_t new_logical = logical + bytes.size();
    const std::size_t end = round_up(new_logical, kDirectIoAlign);
    const std::size_t buf_size = end - start;
    auto buffer = allocate_aligned(kDirectIoAlign, buf_size);
    std::memset(buffer.get(), 0, buf_size);
    if (logical > start) {
      configure_direct(false);
      std::size_t done = 0;
      const std::size_t prefix = logical - start;
      try {
        while (done != prefix) {
          const auto n =
              ::pread(fd_, buffer.get() + done, prefix - done,
                      static_cast<off_t>(start + done));
          if (n < 0) {
            if (errno == EINTR) continue;
            system_failure("read persistence file tail");
          }
          if (n == 0) break;
          done += static_cast<std::size_t>(n);
        }
      } catch (...) {
        configure_direct(true);
        throw;
      }
      configure_direct(true);
    }
    std::memcpy(buffer.get() + (logical - start), bytes.data(), bytes.size());
    std::size_t done = 0;
    while (done != buf_size) {
      const auto n =
          ::pwrite(fd_, buffer.get() + done, buf_size - done,
                   static_cast<off_t>(start + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        system_failure("append persistence file");
      }
      if (n == 0) throw std::runtime_error("zero-byte persistence write");
      done += static_cast<std::size_t>(n);
    }
    if (truncate_fd(fd_, new_logical) != 0) {
      system_failure("truncate persistence file");
    }
    offset_ = new_logical;
  }
#endif

  int fd_{-1};
  std::uint64_t offset_{};
  bool direct_{};
};

void put_u16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value >> 8));
  out.push_back(static_cast<std::byte>(value));
}
void put_u32(Bytes& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::byte>(value >> shift));
}
void put_u64(Bytes& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::byte>(value >> shift));
}
std::uint16_t get_u16(std::span<const std::byte> in, std::size_t at) {
  const auto high = static_cast<std::uint16_t>(
      std::to_integer<std::uint16_t>(in[at]) << 8U);
  return static_cast<std::uint16_t>(
      high | std::to_integer<std::uint16_t>(in[at + 1]));
}
std::uint32_t get_u32(std::span<const std::byte> in, std::size_t at) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i != 4; ++i)
    value = (value << 8) | std::to_integer<std::uint8_t>(in[at + i]);
  return value;
}
std::uint64_t get_u64(std::span<const std::byte> in, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i != 8; ++i)
    value = (value << 8) | std::to_integer<std::uint8_t>(in[at + i]);
  return value;
}
void append_bytes(Bytes& out, std::span<const std::byte> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}
Bytes copy_bytes(std::span<const std::byte> bytes) {
  return Bytes(bytes.begin(), bytes.end());
}
std::span<const std::byte> as_bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
std::runtime_error corrupt(std::string_view kind, std::size_t offset,
                           std::string_view detail) {
  return std::runtime_error(std::string(kind) + " corruption at byte offset " +
                            std::to_string(offset) + ": " + std::string(detail));
}

struct LockedFile {
  explicit LockedFile(File file) : file(std::move(file)) {}
  File file;
  std::mutex mutex;
};

}  // namespace

std::uint32_t crc32(std::span<const std::byte> data) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const std::byte byte : data) {
    crc ^= std::to_integer<std::uint8_t>(byte);
    for (int bit = 0; bit != 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

std::uint32_t crc32(std::string_view data) noexcept { return crc32(as_bytes(data)); }

void write_file(const std::filesystem::path& path,
                std::span<const std::byte> bytes) {
  auto file = File::create_truncated(path);
  file.write_all(bytes);
  file.sync();
}

struct Wal::Impl : LockedFile {
  using LockedFile::LockedFile;
};

Wal::Wal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Wal::~Wal() = default;
Wal::Wal(Wal&&) noexcept = default;
Wal& Wal::operator=(Wal&&) noexcept = default;

std::unique_ptr<Wal> Wal::create(const std::filesystem::path& path) {
  return std::unique_ptr<Wal>(new Wal(std::make_unique<Impl>(File::create(path))));
}

Wal::Recovery Wal::recover(const std::filesystem::path& path) {
  File file = File::open(path);
  const Bytes bytes = file.read_all();
  const std::span<const std::byte> data(bytes);
  std::vector<WalRecord> records;
  std::size_t offset = 0;
  while (offset != data.size()) {
    const std::size_t remaining = data.size() - offset;
    if (remaining < 2) break;
    const std::size_t key_size = get_u16(data, offset);
    if (key_size > remaining - 2 || remaining - 2 - key_size < 2) break;
    const std::size_t value_len_at = offset + 2 + key_size;
    const std::size_t value_size = get_u16(data, value_len_at);
    const std::size_t body_size = 2 + key_size + 2 + value_size;
    if (body_size > remaining || remaining - body_size < 4) break;
    const auto body = data.subspan(offset, body_size);
    if (crc32(body) != get_u32(data, offset + body_size))
      throw corrupt("WAL", offset, "checksum mismatch");
    const auto key = data.subspan(offset + 2, key_size);
    const auto value = data.subspan(value_len_at + 2, value_size);
    if (key.empty() && value.size() >= 4 &&
        value[0] == std::byte{'B'} && value[1] == std::byte{'A'} &&
        value[2] == std::byte{'T'} && value[3] == std::byte{1}) {
      std::size_t cursor = 4;
      while (cursor != value.size()) {
        if (value.size() - cursor < 2)
          throw corrupt("WAL", offset, "invalid batch key length");
        const std::size_t batch_key_size = get_u16(value, cursor);
        cursor += 2;
        if (batch_key_size > value.size() - cursor ||
            value.size() - cursor - batch_key_size < 2)
          throw corrupt("WAL", offset, "invalid batch key");
        auto batch_key = copy_bytes(value.subspan(cursor, batch_key_size));
        cursor += batch_key_size;
        const std::size_t batch_value_size = get_u16(value, cursor);
        cursor += 2;
        if (batch_value_size > value.size() - cursor)
          throw corrupt("WAL", offset, "invalid batch value");
        records.push_back(
            {std::move(batch_key),
             copy_bytes(value.subspan(cursor, batch_value_size))});
        cursor += batch_value_size;
      }
    } else {
      records.push_back({copy_bytes(key), copy_bytes(value)});
    }
    offset += body_size + 4;
  }
  const bool truncated = offset != data.size();
  if (truncated) file.truncate(offset);
  return {std::unique_ptr<Wal>(new Wal(std::make_unique<Impl>(std::move(file)))),
          std::move(records), truncated};
}

void Wal::append(std::span<const std::byte> key, std::span<const std::byte> value) {
  if (key.size() > std::numeric_limits<std::uint16_t>::max())
    throw std::length_error("WAL key is too large");
  if (value.size() > std::numeric_limits<std::uint16_t>::max())
    throw std::length_error("WAL value is too large");
  Bytes record;
  record.reserve(8 + key.size() + value.size());
  put_u16(record, static_cast<std::uint16_t>(key.size()));
  append_bytes(record, key);
  put_u16(record, static_cast<std::uint16_t>(value.size()));
  append_bytes(record, value);
  put_u32(record, crc32(record));
  std::lock_guard lock(impl_->mutex);
  impl_->file.write_all(record);
}
void Wal::append(std::string_view key, std::string_view value) {
  append(as_bytes(key), as_bytes(value));
}
void Wal::append_batch(std::span<const WalRecord> records) {
  if (records.empty()) return;
  Bytes payload{std::byte{'B'}, std::byte{'A'}, std::byte{'T'}, std::byte{1}};
  for (const auto& record : records) {
    if (record.key.empty())
      throw std::invalid_argument("WAL batch keys must not be empty");
    if (record.key.size() > std::numeric_limits<std::uint16_t>::max() ||
        record.value.size() > std::numeric_limits<std::uint16_t>::max())
      throw std::length_error("WAL batch record is too large");
    if (payload.size() > std::numeric_limits<std::uint16_t>::max() -
                             4 - record.key.size() - record.value.size())
      throw std::length_error("WAL batch is too large");
    put_u16(payload, static_cast<std::uint16_t>(record.key.size()));
    append_bytes(payload, record.key);
    put_u16(payload, static_cast<std::uint16_t>(record.value.size()));
    append_bytes(payload, record.value);
  }
  append(std::span<const std::byte>{}, payload);
}
void Wal::sync() {
  std::lock_guard lock(impl_->mutex);
  impl_->file.sync();
}

namespace {

constexpr std::size_t kWalGroupBytes = 64U * 1024U;

std::span<const std::byte> record_key(const MvccWalRecord& record) {
  return record.key;
}
std::span<const std::byte> record_key(const MvccWalRecordView& record) {
  return record.key;
}
std::span<const std::byte> record_value(const MvccWalRecord& record) {
  return record.value;
}
std::span<const std::byte> record_value(const MvccWalRecordView& record) {
  return record.value;
}

template <class Record>
std::size_t mvcc_frame_size(std::span<const Record> records) {
  std::size_t payload_size = 0;
  for (const auto& record : records) {
    const auto key = record_key(record);
    const auto value = record_value(record);
    if (key.size() > std::numeric_limits<std::uint16_t>::max())
      throw std::length_error("MVCC WAL key is too large");
    if (value.size() > std::numeric_limits<std::uint16_t>::max())
      throw std::length_error("MVCC WAL value is too large");
    if (payload_size >
        std::numeric_limits<std::uint32_t>::max() - 12 - key.size() -
            value.size())
      throw std::length_error("MVCC WAL batch is too large");
    payload_size += 12 + key.size() + value.size();
  }
  return payload_size + 8;
}

template <class Record>
void encode_mvcc_frame(std::span<const Record> records,
                       std::span<std::byte> frame) noexcept {
  const auto payload_size = frame.size() - 8;
  std::size_t cursor = 0;
  const auto write_integer = [&](std::uint64_t value, int bytes) {
    for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
      frame[cursor++] = static_cast<std::byte>(value >> shift);
    }
  };
  write_integer(payload_size, 4);
  for (const auto& record : records) {
    const auto key = record_key(record);
    const auto value = record_value(record);
    write_integer(key.size(), 2);
    if (!key.empty())
      std::memcpy(frame.data() + cursor, key.data(), key.size());
    cursor += key.size();
    write_integer(record.timestamp, 8);
    write_integer(value.size(), 2);
    if (!value.empty())
      std::memcpy(frame.data() + cursor, value.data(), value.size());
    cursor += value.size();
  }
  write_integer(crc32(std::span<const std::byte>(frame).subspan(
                    4, payload_size)),
                4);
}

}  // namespace

struct MvccWal::Impl {
  struct Node {
    explicit Node(std::size_t size = 0) : frame_size(size) {}

    static Node* create(std::size_t frame_size) {
      void* storage = ::operator new(sizeof(Node) + frame_size);
      return new (storage) Node(frame_size);
    }

    static void destroy(Node* node) noexcept {
      node->~Node();
      ::operator delete(node);
    }

    [[nodiscard]] std::span<std::byte> frame() noexcept {
      return {reinterpret_cast<std::byte*>(this + 1), frame_size};
    }
    [[nodiscard]] std::span<const std::byte> frame() const noexcept {
      return {reinterpret_cast<const std::byte*>(this + 1), frame_size};
    }

    std::atomic<Node*> next{nullptr};
    std::size_t frame_size{};
  };

  explicit Impl(File file_arg) : file(std::move(file_arg)) {
    stub.next.store(nullptr, std::memory_order_relaxed);
    head = &stub;
    tail.store(&stub, std::memory_order_relaxed);
  }

  ~Impl() {
    try {
      drain(0);
    } catch (...) {
    }
    Node* node = head;
    if (node == &stub) node = stub.next.load(std::memory_order_relaxed);
    while (node != nullptr && node != &stub) {
      Node* next = node->next.load(std::memory_order_relaxed);
      Node::destroy(node);
      node = next;
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  template <class Record>
  void enqueue(std::span<const Record> records) {
    const auto bytes = mvcc_frame_size(records);
    auto* node = Node::create(bytes);
    encode_mvcc_frame(records, node->frame());
    Node* prev = tail.exchange(node, std::memory_order_acq_rel);
    prev->next.store(node, std::memory_order_release);
    queued_bytes.fetch_add(bytes, std::memory_order_release);
  }

  void drain_locked(std::size_t minimum) {
    if (queued_bytes.load(std::memory_order_acquire) < minimum) return;
    drain_buffer.clear();
    Node* current = head;
    Node* next = current->next.load(std::memory_order_acquire);
    while (next != nullptr) {
      const auto frame = next->frame();
      drain_buffer.insert(drain_buffer.end(), frame.begin(), frame.end());
      Node* old = current;
      current = next;
      next = current->next.load(std::memory_order_acquire);
      if (old != &stub) Node::destroy(old);
    }
    head = current;
    const auto taken = drain_buffer.size();
    if (taken != 0) {
      queued_bytes.fetch_sub(taken, std::memory_order_acq_rel);
      file.write_all(drain_buffer);
    }
  }

  void drain(std::size_t minimum) {
    std::lock_guard lock(drain_mutex);
    drain_locked(minimum);
  }

  void try_drain(std::size_t minimum) {
    std::unique_lock lock(drain_mutex, std::try_to_lock);
    if (lock.owns_lock()) drain_locked(minimum);
  }

  File file;
  Node stub;
  Node* head{};
  std::atomic<Node*> tail{nullptr};
  std::atomic<std::size_t> queued_bytes{0};
  std::mutex drain_mutex;
  Bytes drain_buffer;
};

MvccWal::MvccWal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MvccWal::~MvccWal() = default;
MvccWal::MvccWal(MvccWal&&) noexcept = default;
MvccWal& MvccWal::operator=(MvccWal&&) noexcept = default;

std::vector<MvccWalRecord> MvccWal::Recovery::records() const {
  std::vector<MvccWalRecord> flat;
  for (const auto& batch : batches) flat.insert(flat.end(), batch.begin(), batch.end());
  return flat;
}

std::unique_ptr<MvccWal> MvccWal::create(const std::filesystem::path& path) {
  return std::unique_ptr<MvccWal>(
      new MvccWal(std::make_unique<Impl>(File::create(path))));
}

MvccWal::Recovery MvccWal::recover(const std::filesystem::path& path) {
  File file = File::open(path);
  const Bytes bytes = file.read_all();
  const std::span<const std::byte> data(bytes);
  std::vector<std::vector<MvccWalRecord>> batches;
  std::size_t offset = 0;
  while (offset != data.size()) {
    const std::size_t remaining = data.size() - offset;
    if (remaining < 4) break;
    const std::size_t payload_size = get_u32(data, offset);
    if (payload_size > remaining - 4 || remaining - 4 - payload_size < 4) break;
    const auto payload = data.subspan(offset + 4, payload_size);
    if (crc32(payload) != get_u32(data, offset + 4 + payload_size))
      throw corrupt("MVCC WAL", offset, "checksum mismatch");

    std::vector<MvccWalRecord> batch;
    std::size_t cursor = 0;
    while (cursor != payload.size()) {
      if (payload.size() - cursor < 2)
        throw corrupt("MVCC WAL", offset, "incomplete key length in complete frame");
      const std::size_t key_size = get_u16(payload, cursor);
      cursor += 2;
      if (key_size > payload.size() - cursor ||
          payload.size() - cursor - key_size < 10)
        throw corrupt("MVCC WAL", offset, "incomplete key in complete frame");
      MvccWalRecord record;
      record.key = copy_bytes(payload.subspan(cursor, key_size));
      cursor += key_size;
      record.timestamp = get_u64(payload, cursor);
      cursor += 8;
      const std::size_t value_size = get_u16(payload, cursor);
      cursor += 2;
      if (value_size > payload.size() - cursor)
        throw corrupt("MVCC WAL", offset, "incomplete value in complete frame");
      record.value = copy_bytes(payload.subspan(cursor, value_size));
      cursor += value_size;
      batch.push_back(std::move(record));
    }
    batches.push_back(std::move(batch));
    offset += 8 + payload_size;
  }
  const bool truncated = offset != data.size();
  if (truncated) file.truncate(offset);
  return {std::unique_ptr<MvccWal>(
              new MvccWal(std::make_unique<Impl>(std::move(file)))),
          std::move(batches), truncated};
}

void MvccWal::append_batch(std::span<const MvccWalRecord> records) {
  if (records.empty()) return;
  impl_->enqueue(records);
}
void MvccWal::append_batch(std::span<const MvccWalRecordView> records) {
  if (records.empty()) return;
  impl_->enqueue(records);
}
void MvccWal::append(const MvccWalRecord& record) {
  append_batch(std::span<const MvccWalRecord>(&record, 1));
}
void MvccWal::append(MvccWalRecordView record) {
  append_batch(std::span<const MvccWalRecordView>(&record, 1));
}
void MvccWal::flush() { impl_->drain(0); }
void MvccWal::flush_if_needed() {
  impl_->try_drain(kWalGroupBytes);
}
void MvccWal::sync() {
  std::lock_guard lock(impl_->drain_mutex);
  impl_->drain_locked(0);
  impl_->file.sync();
}

Bytes encode_manifest_payload(const InternalManifestRecord& record) {
  if (record.compaction_task.size() > std::numeric_limits<std::uint32_t>::max() ||
      record.output_ids.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("internal manifest payload is too large");
  const auto kind = static_cast<std::uint8_t>(record.kind);
  if (kind < 1 || kind > 3) throw std::invalid_argument("unknown manifest record kind");
  if (record.kind != ManifestRecordKind::compaction &&
      (!record.compaction_task.empty() || !record.output_ids.empty()))
    throw std::invalid_argument("non-compaction manifest record has compaction fields");
  Bytes out{std::byte{'T'}, std::byte{'L'}, std::byte{'S'}, std::byte{'M'},
            std::byte{1}, static_cast<std::byte>(kind), std::byte{0}, std::byte{0}};
  put_u64(out, record.object_id);
  put_u32(out, static_cast<std::uint32_t>(record.compaction_task.size()));
  append_bytes(out, record.compaction_task);
  put_u32(out, static_cast<std::uint32_t>(record.output_ids.size()));
  for (const auto id : record.output_ids) put_u64(out, id);
  return out;
}

InternalManifestRecord decode_manifest_payload(std::span<const std::byte> payload) {
  constexpr std::size_t kFixedSize = 24;
  if (payload.size() < kFixedSize ||
      payload[0] != std::byte{'T'} || payload[1] != std::byte{'L'} ||
      payload[2] != std::byte{'S'} || payload[3] != std::byte{'M'})
    throw std::invalid_argument("not an internal hermesdb manifest payload");
  if (payload[4] != std::byte{1}) throw std::invalid_argument("unsupported manifest payload version");
  if (payload[6] != std::byte{0} || payload[7] != std::byte{0})
    throw std::invalid_argument("invalid manifest payload reserved bytes");
  const auto kind_number = std::to_integer<std::uint8_t>(payload[5]);
  if (kind_number < 1 || kind_number > 3)
    throw std::invalid_argument("unknown manifest record kind");
  InternalManifestRecord result;
  result.kind = static_cast<ManifestRecordKind>(kind_number);
  result.object_id = get_u64(payload, 8);
  const std::size_t task_size = get_u32(payload, 16);
  if (task_size > payload.size() - 20 || payload.size() - 20 - task_size < 4)
    throw std::invalid_argument("truncated internal manifest payload");
  result.compaction_task = copy_bytes(payload.subspan(20, task_size));
  const std::size_t count_at = 20 + task_size;
  const std::size_t count = get_u32(payload, count_at);
  if (count > (payload.size() - count_at - 4) / 8 ||
      count * 8 != payload.size() - count_at - 4)
    throw std::invalid_argument("invalid internal manifest output list");
  std::size_t cursor = count_at + 4;
  for (std::size_t i = 0; i != count; ++i, cursor += 8)
    result.output_ids.push_back(get_u64(payload, cursor));
  if (result.kind != ManifestRecordKind::compaction &&
      (!result.compaction_task.empty() || !result.output_ids.empty()))
    throw std::invalid_argument("non-compaction manifest payload has compaction fields");
  return result;
}

struct Manifest::Impl : LockedFile {
  using LockedFile::LockedFile;
};

Manifest::Manifest(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Manifest::~Manifest() = default;
Manifest::Manifest(Manifest&&) noexcept = default;
Manifest& Manifest::operator=(Manifest&&) noexcept = default;

std::unique_ptr<Manifest> Manifest::create(const std::filesystem::path& path) {
  return std::unique_ptr<Manifest>(
      new Manifest(std::make_unique<Impl>(File::create(path))));
}

Manifest::Recovery Manifest::recover(const std::filesystem::path& path) {
  File file = File::open(path);
  const Bytes bytes = file.read_all();
  const std::span<const std::byte> data(bytes);
  std::vector<ManifestRecord> records;
  std::size_t offset = 0;
  while (offset != data.size()) {
    const std::size_t remaining = data.size() - offset;
    if (remaining < 8) break;
    const std::uint64_t encoded_size = get_u64(data, offset);
    if (encoded_size > std::numeric_limits<std::size_t>::max()) break;
    const std::size_t payload_size = static_cast<std::size_t>(encoded_size);
    if (payload_size > remaining - 8 || remaining - 8 - payload_size < 4) break;
    const auto payload = data.subspan(offset + 8, payload_size);
    if (crc32(payload) != get_u32(data, offset + 8 + payload_size))
      throw corrupt("MANIFEST", offset, "checksum mismatch");
    records.push_back({copy_bytes(payload)});
    offset += 12 + payload_size;
  }
  const bool truncated = offset != data.size();
  if (truncated) file.truncate(offset);
  return {std::unique_ptr<Manifest>(
              new Manifest(std::make_unique<Impl>(std::move(file)))),
          std::move(records), truncated};
}

void Manifest::append(std::span<const std::byte> payload) {
  Bytes frame;
  frame.reserve(payload.size() + 12);
  put_u64(frame, payload.size());
  append_bytes(frame, payload);
  put_u32(frame, crc32(payload));
  std::lock_guard lock(impl_->mutex);
  impl_->file.write_all(frame);
  impl_->file.sync();
}
void Manifest::append(const ManifestRecord& record) { append(record.payload); }
void Manifest::append(const InternalManifestRecord& record) {
  const Bytes payload = encode_manifest_payload(record);
  append(payload);
}
void Manifest::sync() {
  std::lock_guard lock(impl_->mutex);
  impl_->file.sync();
}

}  // namespace hermesdb::persistence
